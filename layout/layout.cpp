#include "layout.h"
#include "font_loader.h"
#include "resource.h"
#include "text_shaper.h"
#include "../net/fetch.h"
#include "../net/url.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <functional>
#include <iostream>
#include <map>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace browser {

static const std::unordered_set<std::string> kNoRender = {
    "title", "meta", "link", "head", "noscript", "template", "script", "style"
};

static bool isCodeContainer(const std::string& tag) {
    return tag == "script" || tag == "style";
}

// The currently-hovered DOM node, used by CSS `:hover` selectors.
static std::shared_ptr<Node> g_hoveredNode;

void setHoveredNode(const std::shared_ptr<Node>& node) {
    g_hoveredNode = node;
}

// ---------------------------------------------------------------------------
// Style resolution (cascade + inheritance + tag defaults + zoom)
// ---------------------------------------------------------------------------

// @media conditions and rule buckets are computed once per (ruleset,
// viewport width) instead of once per node visit — Wikipedia-sized
// sheets have thousands of @media-gated rules and re-parsing them for
// every node dominated layout time.
static std::vector<char> g_mediaOk;

// Rule buckets keyed by the rightmost compound's tag/class/id. For every
// node we only visit the rules whose key can match it (plus the few
// universal ones), turning matching into a handful of map lookups
// instead of a scan of the whole stylesheet.
struct SelCand { uint32_t rule; uint32_t sel; };
struct RuleBuckets {
    const std::vector<CSSRule>* rules = nullptr;
    int width = -1;
    uint64_t generation = 0;   // parseCSS() counter — catches same-address
                               // re-assignments of the rules vector
    std::unordered_map<std::string, std::vector<SelCand>> byKey;
    std::vector<SelCand> any;  // selectors with kind == 0 (match anything)
    std::vector<SelCand> interactive;  // :hover/:focus/... rightmost —
                                       // only visited while interacting
};
static RuleBuckets g_buckets;

// Poor-man's profiling accumulators (printed at the end of layout()).
static double g_tCompute = 0, g_tInsert = 0, g_tWrap = 0;
static long g_nCompute = 0, g_nBoxes = 0, g_nMemoHit = 0;
struct ScopedClock {
    double& acc;
    std::chrono::steady_clock::time_point t0;
    explicit ScopedClock(double& a)
        : acc(a), t0(std::chrono::steady_clock::now()) {}
    ~ScopedClock() {
        acc += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
    }
};

// ---------------------------------------------------------------------------
// computeStyle memo (per layout pass)
//
// The same node's style is requested several times per pass (container
// child loops, inline run collection, table cell measuring, float
// re-layout). Within one pass the base style for a node is always the
// same value, so the result can be cached keyed by the node plus a
// fingerprint of the inheritable base. Cleared on every layout() call —
// that also covers zoom and :hover changes, which always relayout.
// ---------------------------------------------------------------------------
struct StyleMemoEnt {
    uint64_t h1 = 0, h2 = 0;
    Style st;
};
// A node can legitimately be styled against a couple of different bases
// in one pass (e.g. a table cell measured with the table's style, then
// laid out with its own). Keep the last few so alternating callers hit.
struct StyleMemoVec {
    StyleMemoEnt ents[3];
    int n = 0;
};
static std::unordered_map<const void*, StyleMemoVec> g_styleMemo;

static uint64_t fnv1a(uint64_t h, const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;
    for (size_t i = 0; i < len; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

// Fingerprint of the base fields a child's computed style can depend on.
static void baseFingerprint(const Style& b, uint64_t& h1, uint64_t& h2) {
    h1 = 1469598103934665603ull;
    h1 = fnv1a(h1, &b.declared, sizeof(b.declared));
    h1 = fnv1a(h1, &b.fontSize, sizeof(b.fontSize));
    h1 = fnv1a(h1, &b.bold, sizeof(b.bold));
    h1 = fnv1a(h1, &b.italic, sizeof(b.italic));
    h1 = fnv1a(h1, &b.underline, sizeof(b.underline));
    h1 = fnv1a(h1, &b.lineHeight, sizeof(b.lineHeight));
    h1 = fnv1a(h1, &b.color, sizeof(b.color));
    h1 = fnv1a(h1, &b.pre, sizeof(b.pre));
    h2 = 1469598103934665603ull;
    h2 = fnv1a(h2, b.fontFamily.data(), b.fontFamily.size());
    h2 = fnv1a(h2, &b.textTransform, sizeof(b.textTransform));
    h2 = fnv1a(h2, &b.hasDirection, sizeof(b.hasDirection));
    h2 = fnv1a(h2, b.direction.data(), b.direction.size());
}

static void buildRuleBuckets(const std::vector<CSSRule>& rules) {
    g_buckets.rules = &rules;
    g_buckets.width = mediaViewportWidth();
    g_buckets.generation = parseCSSGeneration();
    g_buckets.byKey.clear();
    g_buckets.any.clear();
    g_buckets.interactive.clear();
    g_mediaOk.assign(rules.size(), 0);
    uint32_t n = (uint32_t)rules.size();
    for (uint32_t ri = 0; ri < n; ++ri) {
        const CSSRule& rule = rules[ri];
        g_mediaOk[ri] = rule.media.empty() ||
                        mediaConditionMatches(rule.media);
        for (uint32_t si = 0; si < rule.selectors.size(); ++si) {
            char kind = 0;
            std::string key;
            if (si < rule.selKeys.size()) {
                kind = rule.selKeys[si].kind;
                key = rule.selKeys[si].key;
            }
            SelCand c{ri, si};
            if (kind == 't') {
                g_buckets.byKey["t:" + key].push_back(c);
            } else if (kind == 'c') {
                g_buckets.byKey["c:" + key].push_back(c);
            } else if (kind == 'i') {
                g_buckets.byKey["i:" + key].push_back(c);
            } else if (kind == 'a') {
                g_buckets.byKey["a:" + key].push_back(c);
            } else if (kind == 'h') {
                g_buckets.interactive.push_back(c);
            } else if (kind == 'r') {
                g_buckets.byKey["r:" + key].push_back(c);
            } else if (kind == 'n') {
                // rightmost pseudo-element / empty :is() — never renders
            } else {
                g_buckets.any.push_back(c);
            }
        }
    }
    if (getenv("MB_LAYOUTTRACE")) {
        size_t biggest = 0; std::string bk;
        for (auto& [k, v] : g_buckets.byKey)
            if (v.size() > biggest) { biggest = v.size(); bk = k; }
        std::fprintf(stderr,
            "[buckets] rules=%zu any=%zu interactive=%zu keys=%zu"
            " biggest='%s'(%zu)\n", rules.size(), g_buckets.any.size(),
            g_buckets.interactive.size(), g_buckets.byKey.size(),
            bk.c_str(), biggest);
        int shown = 0;
        for (auto& c : g_buckets.any) {
            if (shown++ >= 25) break;
            std::fprintf(stderr, "[any] %s\n",
                         rules[c.rule].selectors[c.sel].c_str());
        }
    }
}

static Style computeStyle(const Style& base,
                          const std::shared_ptr<Node>& node,
                          const std::vector<CSSRule>& cssRules);

// Optional per-call style overlay applied at the END of computeStyle (via
// mergeDeclared, so only its declared properties land). Used by the table
// layout to give cells their border=1 frame without resorting to
// inheritance (the box model no longer leaks down the Style chain).
static Style* g_styleOverlay = nullptr;

static Style computeStyle(const Style& base,
                          const std::shared_ptr<Node>& node,
                          const std::vector<CSSRule>& cssRules) {
    ScopedClock _clk(g_tCompute); ++g_nCompute;

    // Memo lookup: same node + same inheritable-base fingerprint within
    // this layout pass returns the cached result.
    uint64_t h1 = 0, h2 = 0;
    baseFingerprint(base, h1, h2);
    if (g_styleOverlay) h1 ^= 0x9E3779B97F4A7C15ull;   // overlay changes result
    {
        auto mit = g_styleMemo.find(node.get());
        if (mit != g_styleMemo.end()) {
            for (int i = 0; i < mit->second.n; ++i) {
                if (mit->second.ents[i].h1 == h1 &&
                    mit->second.ents[i].h2 == h2) {
                    ++g_nMemoHit;
                    return mit->second.ents[i].st;
                }
            }
        }
    }

    Style s = base;
    // Non-inherited properties must not leak down the Style-copy chain.
    // CSS inherits only text-ish properties (color, fonts, line-height,
    // text-align/transform, white-space, visibility, list-style,
    // direction). The box model (margin/padding/border/size/position/
    // float/background/opacity/flex/grid/display) used to flow to every
    // descendant: body's 8px margin silently became an 8px margin on
    // EVERY element, a body border framed every block, and display:grid
    // on a container recursively turned all children into grid
    // containers. Reset them; rules/inline styles/tag defaults of THIS
    // node set them again below.
    s.display = "inline";
    s.hasDisplay = false;
    s.margin = Edges{};
    s.marginPct = PctEdges{};
    s.marginAutoLeft = s.marginAutoRight = false;
    s.hasMargin = false;
    s.padding = Edges{};
    s.paddingPct = PctEdges{};
    s.hasPadding = false;
    s.border = Edges{};
    s.borderColor = s.borderTopColor = s.borderRightColor =
        s.borderBottomColor = s.borderLeftColor = {0, 0, 0, 255};
    s.hasBorder = s.hasBorderColor = false;
    s.width = s.height = s.maxWidth = s.minWidth = -1;
    s.maxHeight = s.minHeight = -1;
    s.widthPct = s.heightPct = s.maxWidthPct = s.minWidthPct = 0;
    s.hasWidth = s.hasHeight = s.hasMaxWidth = s.hasMinWidth = false;
    s.hasWidthPct = s.hasHeightPct = false;
    s.hasMaxWidthPct = s.hasMinWidthPct = false;
    s.hasMaxHeight = s.hasMinHeight = false;
    s.position = "static";
    s.hasPosition = false;
    s.offTop = s.offRight = s.offBottom = s.offLeft = 0;
    s.offTopPct = s.offRightPct = s.offBottomPct = s.offLeftPct = 0;
    s.hasOffTop = s.hasOffRight = s.hasOffBottom = s.hasOffLeft = false;
    s.cssFloat = "none";
    s.hasFloat = false;
    s.boxSizing = "content-box";
    s.hasBoxSizing = false;
    s.objectFit = "fill";
    s.hasObjectFit = false;
    s.opacity = 1.0f;
    s.hasOpacity = false;
    s.bg = {255, 255, 255, 255};
    s.hasBg = false;
    s.bgImageUrl.clear();
    s.hasBgImage = false;
    s.verticalAlign = "baseline";
    s.hasVerticalAlign = false;
    s.flexDirection = "row";
    s.justifyContent = "flex-start";
    s.alignItems = "stretch";
    s.flexGap = 0;
    s.hasFlexGap = false;
    s.gridTemplateColumns.clear();
    s.gridTemplateRows.clear();
    s.gridTemplateAreas.clear();
    s.gridArea.clear();
    s.hasGridTemplateColumns = s.hasGridTemplateRows = false;
    s.hasGridTemplateAreas = s.hasGridArea = false;
    s.declared &= ~(B_DISPLAY | B_BG | B_BG_IMAGE | B_BG_SIZE | B_BG_REPEAT |
                    B_BG_POS | B_MARGIN | B_PADDING | B_BORDER |
                    B_BORDER_COLOR | B_WIDTH | B_HEIGHT | B_MAX_WIDTH |
                    B_MIN_WIDTH | B_MAX_HEIGHT | B_MIN_HEIGHT | B_POSITION |
                    B_OFFSETS | B_FLOAT | B_FLEX_DIR | B_JUSTIFY |
                    B_ALIGN_ITEMS | B_GAP | B_BOX_SIZING | B_OBJECT_FIT |
                    B_OPACITY | B_VERTICAL_ALIGN | B_GRID_TPL | B_GRID_AREAS |
                    B_GRID_AREA);
    // True when THIS call actually assigned a font size (via a matching
    // rule, inline style, or tag default) — inherited sizes must not be
    // re-zoomed or zoom compounds with nesting depth.
    bool sizeSetHere = false;

    if (&cssRules != g_buckets.rules ||
        g_buckets.width != mediaViewportWidth() ||
        g_buckets.generation != parseCSSGeneration() ||
        g_mediaOk.size() != cssRules.size()) {
        buildRuleBuckets(cssRules);
    }

    // Gather this node's candidate (rule, selector) pairs from the
    // buckets, then apply them in rule order (rules are sorted by
    // specificity, so ascending rule index preserves the cascade).
    // A rule reached through several matching selectors just re-applies
    // the same declarations, which is idempotent.
    thread_local std::vector<SelCand> cands;
    cands.clear();
    {
        auto append = [&](const std::vector<SelCand>& v) {
            cands.insert(cands.end(), v.begin(), v.end());
        };
        append(g_buckets.any);
        // :root rules match only the document root node.
        if (node->parent.lock() == nullptr) {
            auto rt = g_buckets.byKey.find("r:root");
            if (rt != g_buckets.byKey.end()) append(rt->second);
        }
        // Interactive pseudo rules matter only for the hover/focus
        // relayout; static layout skips thousands of them.
        if (g_hoveredNode)
            append(g_buckets.interactive);
        auto it = g_buckets.byKey.find("t:" + node->tag);
        if (it != g_buckets.byKey.end()) append(it->second);
        auto cit = node->attrs.find("class");
        if (cit != node->attrs.end() && !cit->second.empty()) {
            const std::string& cl = cit->second;
            size_t p = 0;
            while (p < cl.size()) {
                size_t e = cl.find_first_of(" \t\r\n", p);
                if (e == std::string::npos) e = cl.size();
                if (e > p) {
                    std::string cls = "c:" + cl.substr(p, e - p);
                    auto kt = g_buckets.byKey.find(cls);
                    if (kt != g_buckets.byKey.end()) append(kt->second);
                }
                p = e + 1;
            }
        }
        auto iit = node->attrs.find("id");
        if (iit != node->attrs.end()) {
            auto kt = g_buckets.byKey.find("i:" + iit->second);
            if (kt != g_buckets.byKey.end()) append(kt->second);
        }
        // Attribute-keyed rules: only those whose required attribute the
        // node actually carries.
        for (auto& [aname, aval] : node->attrs) {
            (void)aval;
            auto kt = g_buckets.byKey.find("a:" + aname);
            if (kt != g_buckets.byKey.end()) append(kt->second);
        }
    }
    std::sort(cands.begin(), cands.end(), [](const SelCand& a, const SelCand& b) {
        return a.rule != b.rule ? a.rule < b.rule : a.sel < b.sel;
    });

    static long g_csCalls = 0;
    static auto g_csT0 = std::chrono::steady_clock::now();
    if (++g_csCalls % 2000 == 0 && getenv("MB_LAYOUTTRACE")) {
        double el = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - g_csT0).count();
        std::fprintf(stderr, "[cs] calls=%ld elapsed=%.0fms cands=%zu"
                             " tag=%s\n", g_csCalls, el, cands.size(),
                     node->tag.c_str());
    }

    for (const SelCand& c : cands) {
        const CSSRule& rule = cssRules[c.rule];
        if (!g_mediaOk[c.rule]) continue;
        bool matched;
        if (c.sel < rule.selItems.size() && !rule.selItems[c.sel].empty()) {
            matched = matchesSelectorItems(rule.selItems[c.sel], node,
                                           g_hoveredNode);
        } else if (c.sel < rule.selTokens.size()) {
            matched = matchesSelectorTokens(rule.selTokens[c.sel], node,
                                            g_hoveredNode);
        } else {
            matched = matchesSelectorNodeHover(rule.selectors[c.sel], node,
                                               g_hoveredNode);
        }
        if (matched) {
            mergeDeclared(s, rule.style);
            if (rule.style.declared & B_FONT_SIZE) sizeSetHere = true;
        }
    }

    if (node->attrs.count("style")) {
        int fsBefore = s.fontSize;
        s = applyStyle(s, node->attrs["style"]);
        if (s.fontSize != fsBefore) sizeSetHere = true;
    }

    // dir attribute (presentational hint): applies only when no CSS
    // rule/inline style declared `direction`. dir=auto is kept as-is and
    // resolved per box by first-strong detection at wrap time.
    if (!s.hasDirection) {
        auto dit = node->attrs.find("dir");
        if (dit != node->attrs.end()) {
            std::string dv = dit->second;
            for (auto& ch : dv) ch = (char)tolower((unsigned char)ch);
            if (dv == "rtl" || dv == "ltr" || dv == "auto") {
                s.direction = dv;
                s.hasDirection = true;
            }
        }
    }

    if      (node->tag == "h1") { s.fontSize = 32; s.bold = true; s.hasFontSize = true; s.hasBold = true; sizeSetHere = true; }
    else if (node->tag == "h2") { s.fontSize = 24; s.bold = true; s.hasFontSize = true; s.hasBold = true; sizeSetHere = true; }
    else if (node->tag == "h3") { s.fontSize = 20; s.bold = true; s.hasFontSize = true; s.hasBold = true; sizeSetHere = true; }
    else if (node->tag == "h4") { s.fontSize = 18; s.bold = true; s.hasFontSize = true; s.hasBold = true; sizeSetHere = true; }
    else if (node->tag == "h5") { s.fontSize = 16; s.bold = true; s.hasBold = true; }
    else if (node->tag == "h6") { s.fontSize = 14; s.bold = true; s.hasFontSize = true; s.hasBold = true; sizeSetHere = true; }
    else if (node->tag == "b" || node->tag == "strong") { s.bold = true; s.hasBold = true; }
    else if (node->tag == "i" || node->tag == "em")     { s.italic = true; s.hasItalic = true; }
    else if (node->tag == "u" || node->tag == "ins")    { s.underline = true; s.hasUnderline = true; }
    else if (node->tag == "code" || node->tag == "kbd" || node->tag == "samp" || node->tag == "tt") {
        s.fontFamily = "monospace";
        s.hasFontFamily = true;
    }
    else if (node->tag == "pre") {
        s.fontFamily = "monospace";
        s.hasFontFamily = true;
        s.bg = {240, 240, 240, 255};
        s.hasBg = true;
        s.pre = true;
        s.hasPre = true;
    }
    else if (node->tag == "dt") { s.bold = true; s.hasBold = true; }
    else if (node->tag == "th") { s.bold = true; s.hasBold = true; }
    else if (node->tag == "small") { s.fontSize = std::max(9, s.fontSize * 13 / 16); sizeSetHere = true; }
    else if (node->tag == "big")   { s.fontSize = s.fontSize * 12 / 10; sizeSetHere = true; }
    else if (node->tag == "sub" || node->tag == "sup") {
        s.fontSize = std::max(9, s.fontSize * 3 / 4);
        sizeSetHere = true;
    }
    else if (node->tag == "mark") {
        s.bg = {255, 255, 128, 255};
        s.hasBg = true;
    }
    else if (node->tag == "del" || node->tag == "s" || node->tag == "strike") {
        // strikethrough not rasterized; render slightly lighter
        s.color = {110, 110, 110, 255};
        s.hasColor = true;
    }

    if (node->tag == "a" && node->attrs.count("href")) {
        // UA-default link look — but the cascade rules: an author color
        // (stylesheet or inline style, both merged above with their
        // `declared` bits) must win, exactly like a real browser. This is
        // what lets the YouTube shim draw its white-on-red Play button.
        if (!(s.declared & B_COLOR)) {
            s.color = {0x1a, 0x4f, 0xa0, 255};
            s.hasColor = true;
        }
        if (!(s.declared & B_UNDERLINE)) {
            s.underline = true;
            s.hasUnderline = true;
        }
    }

    // Default margins for common block tags (real UA stylesheet). Only
    // applied when no CSS margin was declared — resets win.
    if (!(s.declared & B_MARGIN)) {
        float emT = 0, emB = 0;
        const std::string& t = node->tag;
        if (t == "p")                        { emT = emB = 1.0f; }
        else if (t == "h1")                  { emT = emB = 0.67f; }
        else if (t == "h2")                  { emT = emB = 0.75f; }
        else if (t == "h3")                  { emT = emB = 0.83f; }
        else if (t == "h4" || t == "h5")     { emT = emB = 1.0f; }
        else if (t == "h6")                  { emT = emB = 1.2f; }
        else if (t == "ul" || t == "ol")     { emT = emB = 1.0f; }
        else if (t == "dl")                  { emT = emB = 1.0f; }
        else if (t == "blockquote")          { emT = emB = 1.0f; }
        else if (t == "figure")              { emT = emB = 1.0f; }
        else if (t == "pre")                 { emT = emB = 1.0f; }
        else if (t == "address")             { emT = emB = 1.0f; }
        else if (t == "table")               { emT = 0; emB = 0; }
        else if (t == "figcaption")          { emT = 0.4f; emB = 0.8f; }
        else if (t == "form")                { emT = 0; emB = 0; }
        if (emT != 0 || emB != 0) {
            s.margin.top = (int)std::lround(emT * s.fontSize);
            s.margin.bottom = (int)std::lround(emB * s.fontSize);
            s.hasMargin = true;
            s.declared |= B_MARGIN;
        }
    }
    // <td>/<th> default padding (cell insets).
    if ((node->tag == "td" || node->tag == "th") && !(s.declared & B_PADDING)) {
        s.padding.set1(3);
        s.hasPadding = true;
        s.declared |= B_PADDING;
    }

    // Apply the global page zoom to NEWLY-SET font sizes only.
    float z = getGlobalZoom();
    if (sizeSetHere && z != 1.0f && z > 0.0f) {
        int scaled = (int)std::lround(s.fontSize * z);
        if (scaled < 8) scaled = 8;
        s.fontSize = scaled;
        if (s.hasLineHeight && s.lineHeight > 0)
            s.lineHeight = std::lround(s.lineHeight * z);
    }

    // Style overlay (see g_styleOverlay below): applied LAST so it wins
    // over everything, letting callers inject per-call attributes such as
    // a table's border=1 onto cells without abusing inheritance.
    if (g_styleOverlay) mergeDeclared(s, *g_styleOverlay);

    {
        StyleMemoVec& vec = g_styleMemo[node.get()];
        if (vec.n < 3) {
            vec.ents[vec.n++] = StyleMemoEnt{h1, h2, s};
        } else {
            vec.ents[0] = std::move(vec.ents[1]);
            vec.ents[1] = std::move(vec.ents[2]);
            vec.ents[2] = StyleMemoEnt{h1, h2, s};
        }
    }
    return s;
}

static TTF_Font* getFontFor(const Style& s, TTF_Font* defaultFont) {
    bool nonDefault = (s.fontFamily != "sans-serif")
                   || (s.fontSize != 16)
                   || s.italic
                   || s.bold;
    if (!nonDefault) return defaultFont;
    if (TTF_Font* sized = getFontForFamily(s.fontFamily, s.fontSize, s.bold, s.italic))
        return sized;
    return defaultFont;
}

// Apply CSS text-transform to a text run (ASCII letters).
static std::string applyTextTransform(const std::string& text,
                                      const std::string& mode) {
    if (mode.empty() || mode == "none") return text;
    if (mode == "uppercase") {
        std::string out = text;
        for (auto& c : out)
            if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        return out;
    }
    if (mode == "lowercase") {
        std::string out = text;
        for (auto& c : out)
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        return out;
    }
    if (mode == "capitalize") {
        std::string out = text;
        bool wordStart = true;
        for (auto& c : out) {
            if (std::isspace((unsigned char)c)) { wordStart = true; }
            else if (wordStart && c >= 'a' && c <= 'z') {
                c = (char)(c - 'a' + 'A');
                wordStart = false;
            } else {
                wordStart = false;
            }
        }
        return out;
    }
    return text;
}

// ---------------------------------------------------------------------------
// Float regions (per block container)
// ---------------------------------------------------------------------------

struct FloatRegion {
    bool isLeft = true;
    int x = 0, w = 0;      // occupied horizontal span
    int yTop = 0, yBottom = 0;  // occupied vertical span (absolute coords)
};

// Extra left inset at vertical span [y, y+h).
static int floatLeftInset(const std::vector<FloatRegion>& fl, int y, int h) {
    int inset = 0;
    for (auto& f : fl) {
        if (f.isLeft && f.yBottom > y && f.yTop < y + h)
            inset = std::max(inset, f.x + f.w);
    }
    return inset;
}

// Right edge (absolute x) that right-floating content occupies.
static int floatRightEdge(const std::vector<FloatRegion>& fl, int y, int h) {
    int edge = 1 << 20;
    bool any = false;
    for (auto& f : fl) {
        if (!f.isLeft && f.yBottom > y && f.yTop < y + h) {
            edge = std::min(edge, f.x);
            any = true;
        }
    }
    return any ? edge : (1 << 20);
}

// Vertical drop that keeps a box's text readable (CSS 2.1 §9.5.2): when
// overlapping floats crush the usable line width under ~100px, line boxes
// move below the floats instead of wrapping into one-character columns.
// Returns 0 when no drop is needed. `probeH` should cover the box's first
// few lines so partial overlaps (text around an image tail) stay intact.
static int floatCrushDropDy(const std::vector<FloatRegion>& floats,
                            int contentX, int contentW, int y, int probeH) {
    if (floats.empty() || contentW <= 0) return 0;
    const int kMinUsable = 100;
    int lAbs = floatLeftInset(floats, y, probeH);
    int lIns = lAbs - contentX;
    int reEdge = floatRightEdge(floats, y, probeH);
    int rIns = (reEdge < (1 << 20))
                   ? std::max(0, contentX + contentW - reEdge) : 0;
    if (!((lIns > 0 || rIns > 0) && contentW - lIns - rIns < kMinUsable))
        return 0;
    int below = y;
    for (const auto& f : floats)
        if (f.yBottom > y && f.yTop < y + probeH)
            below = std::max(below, f.yBottom);
    if (below > y && below - y < 2000) return below + 2 - y;  // sanity cap
    return 0;
}

// ---------------------------------------------------------------------------
// Image preparation (shared by block and inline image paths)
// ---------------------------------------------------------------------------

static int attrInt(const std::shared_ptr<Node>& n, const std::string& key) {
    auto it = n->attrs.find(key);
    if (it == n->attrs.end()) return 0;
    try { return std::stoi(it->second); } catch (...) { return 0; }
}

static void prepareImage(const std::shared_ptr<Node>& node,
                         const Style& cssStyle,
                         const std::vector<CSSRule>& cssRules,
                         int availW,
                         std::string& outPath,
                         int& outNatW, int& outNatH,
                         int& outDrawW, int& outDrawH,
                         std::string& outAlt) {
    outPath = pickImageSrc(node);
    outAlt = node->attrs.count("alt") ? node->attrs["alt"] : "";
    outNatW = outNatH = outDrawW = outDrawH = 0;

    Style imgStyle = cssStyle;
    if (&cssStyle == &cssStyle) {}  // no-op; cssStyle already node's style
    // CSS width/height may carry % parts — resolve against availW.
    int cssW = -1, cssH = -1, cssMaxW = -1;
    if (imgStyle.hasWidth && imgStyle.width >= 0)
        cssW = imgStyle.width + (int)std::lround(imgStyle.widthPct * availW / 100.0f);
    if (imgStyle.hasHeight && imgStyle.height >= 0)
        cssH = imgStyle.height;
    if (imgStyle.hasMaxWidth && imgStyle.maxWidth >= 0)
        cssMaxW = imgStyle.maxWidth +
                  (int)std::lround(imgStyle.maxWidthPct * availW / 100.0f);

    if (outPath.empty()) return;

    CachedResource* img = ResourceLoader::instance().loadImage(outPath);
    int natW = 0, natH = 0;
    if (img && img->surface) {
        natW = img->w;
        natH = img->h;
    }
    outNatW = natW;
    outNatH = natH;

    int reqW = attrInt(node, "width");
    int reqH = attrInt(node, "height");

    int dw = 0, dh = 0;
    if (natW > 0 && natH > 0) {
        if      (cssW > 0 && cssH > 0) { dw = cssW; dh = cssH; }
        else if (reqW > 0 && reqH > 0) { dw = reqW; dh = reqH; }
        else if (cssW > 0)             { dw = cssW; dh = cssW * natH / natW; }
        else if (cssH > 0)             { dh = cssH; dw = cssH * natW / natH; }
        else if (reqW > 0)             { dw = reqW; dh = reqW * natH / natW; }
        else if (reqH > 0)             { dh = reqH; dw = reqH * natW / natH; }
        else                           { dw = natW; dh = natH; }

        // max-width clamp (CSS), then a hard responsive clamp so no image
        // overflows its column — modern pages assume max-width:100%.
        int maxW = dw;
        if (cssMaxW > 0) maxW = std::min(maxW, cssMaxW);
        if (availW > 0)  maxW = std::min(maxW, availW);
        if (dw > maxW && dw > 0) {
            dh = std::max(1, dh * maxW / dw);
            dw = maxW;
        }
    } else {
        // Broken/missing image: honor requested box so the alt placeholder
        // occupies sane space.
        dw = std::max(20, std::max(reqW, cssW > 0 ? cssW : 0));
        dh = std::max(20, std::max(reqH, cssH > 0 ? cssH : 0));
        if (availW > 0 && dw > availW) dw = availW;
    }
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    outDrawW = dw;
    outDrawH = dh;
}

// ---------------------------------------------------------------------------
// Text run collection (with inline images)
// ---------------------------------------------------------------------------

// Tags that flow as inline content inside a paragraph.
static const std::unordered_set<std::string> kInlineTags = {
    "text", "a", "b", "i", "strong", "em", "span", "code", "br",
    "u", "s", "small", "sub", "sup", "label", "mark", "del", "ins",
    "big", "tt", "kbd", "samp", "cite", "q", "abbr", "time", "font",
    "img", "picture", "wbr", "bdi", "bdo", "data", "var"
};

// A block element encountered while walking INLINE content ("block in
// inline", e.g. Wikipedia's <span><div class="itn-img" style="float:right">
// …</div></span>). Recorded in document order so the container can
// interleave real block layout between the inline run segments instead of
// flattening the block into the text flow (which lost float / display
// semantics and turned floated image thumbs into full-width inline boxes).
struct InlineBlockSpot {
    size_t runPos;                    // index into the run vector (marker)
    std::shared_ptr<Node> node;       // the block node to lay out
    Style parentStyle;                // resolved style of its inline parents
};

static void collectRuns(const std::shared_ptr<Node>& node,
                        const Style& parentStyle,
                        bool parentBold, bool parentItalic,
                        const std::string& parentHref,
                        const std::weak_ptr<Node>& parentLinkNode,
                        const std::vector<CSSRule>& cssRules,
                        int availW,
                        std::vector<Run>& runs,
                        bool inInlineFlow = false,
                        std::vector<InlineBlockSpot>* inlineBlocks = nullptr) {
    bool myBold   = parentBold
        || node->tag == "b" || node->tag == "strong"
        || node->tag == "h1" || node->tag == "h2" || node->tag == "h3";
    bool myItalic = parentItalic
        || node->tag == "i" || node->tag == "em";

    Style myStyle = (node->tag == "text")
        ? parentStyle
        : computeStyle(parentStyle, node, cssRules);

    // display:none inside inline flow contributes nothing.
    if (inInlineFlow && myStyle.display == "none") return;
    // Out-of-flow elements are laid out by the container's absolute pass —
    // contributing an inline run here would paint them twice (BBC's
    // offscreen "Skip to content" link re-appeared at the page top).
    if (myStyle.position == "absolute" || myStyle.position == "fixed") return;
    // visibility:hidden / sr-only clip idioms contribute nothing either
    // (their text would otherwise leak into the parent's run stream —
    // the "Skip to content" / "Navigation Menu" letter columns).
    if (myStyle.visibilityHidden || myStyle.clippedAway) return;

    std::string myHref = parentHref;
    std::weak_ptr<Node> myLinkNode = parentLinkNode;
    if (node->tag == "a" && node->attrs.count("href")) {
        myHref = node->attrs["href"];
        myLinkNode = node;
    }

    // Inline images become atomic image runs.
    if (node->tag == "img") {
        // Floated images are handled by the container, not the run flow.
        if (myStyle.cssFloat != "none") return;
        Run r;
        r.isImage = true;
        r.style = myStyle;
        r.isLink = !myHref.empty();
        r.href   = myHref;
        r.linkNode = myLinkNode;
        r.sourceNode = node;   // the <img> itself
        std::string natWTrash;
        int natW = 0, natH = 0;
        prepareImage(node, myStyle, cssRules, availW,
                     r.imagePath, natW, natH, r.drawW, r.drawH, r.altText);
        (void)natWTrash;
        runs.push_back(r);
        return;
    }

    for (auto& c : node->children) {
        // Inside a block container, only inline content flows into text
        // runs — block children (p, div, ul, table, ...) lay themselves
        // out separately, so recursing into them here would duplicate
        // every nested paragraph. INSIDE an inline element, though, a
        // block child is "block-in-inline". Real browsers split the
        // inline box around it and lay the child out as a block; we
        // record it as an InlineBlockSpot (plus a hard-break marker run
        // so the segments wrap as separate lines) and the container
        // interleaves it into the flow in document order. Floated
        // block-in-inline (Wikipedia thumbs) keeps its float semantics
        // this way.
        if (!kInlineTags.count(c->tag)) {
            // Inline <svg> icons have no renderable text for us; script/
            // style are covered by kNoRender.
            static const std::unordered_set<std::string> kInlineSkipExtra = {
                "svg", "math"
            };
            if (inInlineFlow && !c->children.empty() &&
                !kNoRender.count(c->tag) &&
                !kInlineSkipExtra.count(c->tag)) {
                Style cStyle = computeStyle(myStyle, c, cssRules);
                if (!(cStyle.hasDisplay && cStyle.display == "none") &&
                    !cStyle.visibilityHidden && !cStyle.clippedAway) {
                    static const std::unordered_set<std::string>
                        kBlockByDefault = {
                            "div", "p", "ul", "ol", "dl", "table", "section",
                            "article", "aside", "header", "footer", "nav",
                            "main", "figure", "figcaption", "fieldset",
                            "form", "details", "summary", "h1", "h2", "h3",
                            "h4", "h5", "h6", "blockquote", "pre", "hr",
                            "center", "video", "audio", "canvas", "iframe"
                        };
                    bool treatAsBlock =
                        cStyle.cssFloat != "none" ||
                        (cStyle.hasDisplay && cStyle.display != "inline") ||
                        kBlockByDefault.count(c->tag) > 0;
                    if (treatAsBlock && inlineBlocks) {
                        inlineBlocks->push_back(
                            InlineBlockSpot{runs.size(), c, myStyle});
                        Run marker;   // hard break between the segments
                        marker.text = "\n";
                        marker.style = myStyle;
                        marker.isLink = !myHref.empty();
                        marker.href   = myHref;
                        marker.linkNode = myLinkNode;
                        // Attribute the break to the block child, not to
                        // the inline ancestors — the child lays itself out
                        // as its own box, and inline-fragment rects must
                        // not grow to cover it.
                        marker.sourceNode = c;
                        runs.push_back(marker);
                    } else {
                        // Legacy measuring path (no spot list) or a true
                        // display:inline element: transparent unwrap.
                        collectRuns(c, myStyle, myBold, myItalic, myHref,
                                    myLinkNode, cssRules, availW, runs,
                                    true, inlineBlocks);
                    }
                }
            }
            continue;
        }
        if (c->tag == "br") {
            Run r;
            r.text = "\n";
            r.style = myStyle;
            r.isLink = !myHref.empty();
            r.href   = myHref;
            r.linkNode = myLinkNode;
            r.sourceNode = c;   // the <br>
            runs.push_back(r);
        } else if (c->tag == "text") {
            Run r;
            r.text = applyTextTransform(c->text, myStyle.textTransform);
            r.style = myStyle;
            r.style.bold   = r.style.bold   || myBold;
            r.style.italic = r.style.italic || myItalic;
            r.isLink = !myHref.empty();
            r.href   = myHref;
            r.linkNode = myLinkNode;
            r.sourceNode = node;   // nearest element ancestor of the text
            runs.push_back(r);
        } else {
            collectRuns(c, myStyle, myBold, myItalic, myHref, myLinkNode,
                        cssRules, availW, runs, true, inlineBlocks);
        }
    }
}

// ---------------------------------------------------------------------------
// Line wrapping engine (text + inline images, float aware)
// ---------------------------------------------------------------------------

struct WrapLine {
    std::vector<Run> runs;
    int height = 0;   // line box height (text line-height or tallest image)
    // Float insets (relative to the content box) active for this line.
    int insetLeft = 0, insetRight = 0;
};

struct WrapResult {
    std::vector<WrapLine> lines;
    int maxLineWidth = 0;
};

// Wrap `runs` into lines inside a content column that starts at absolute x
// `contentX`, is `contentW` wide, begins at absolute y `startY`, with
// per-line height `lineH` for text. Floats shrink lines that overlap.
static WrapResult wrapRuns(const std::vector<Run>& runs,
                           int contentX, int contentW, int startY,
                           int lineH,
                           const std::vector<FloatRegion>& floats,
                           const Style& s) {
    ScopedClock _clkW(g_tWrap);
    WrapResult out;
    std::vector<Run> currentLine;
    int currentWidth = 0;
    int currentLineH = lineH;
    int lineY = startY;
    bool atLineStart = true;

    auto stylesMatch = [](const Style& a, const Style& b) {
        return a.bold == b.bold && a.italic == b.italic
            && a.fontSize == b.fontSize
            && a.color.r == b.color.r && a.color.g == b.color.g
            && a.color.b == b.color.b
            && a.underline == b.underline
            && a.fontFamily == b.fontFamily;
    };

    // Float insets for the line starting at absolute y: the renderer
    // shifts/shortens each line by these (WrapLine/Box::lineInsets).
    int curInsetL = 0, curInsetR = 0;
    auto updateInsets = [&]() {
        curInsetL = std::max(0, floatLeftInset(floats, lineY, lineH) -
                                  contentX);
        int rightEdge = floatRightEdge(floats, lineY, lineH);
        curInsetR = (rightEdge < (1 << 20))
                        ? std::max(0, contentX + contentW - rightEdge)
                        : 0;
    };
    updateInsets();

    auto flushLine = [&]() {
        if (currentLine.empty()) {
            // Keep explicit empty lines only for hard breaks; the caller
            // handles <br> via a "\n" run, so just reset metrics.
        } else {
            out.lines.push_back(WrapLine{currentLine, currentLineH,
                                         curInsetL, curInsetR});
        }
        out.maxLineWidth = std::max(out.maxLineWidth, currentWidth);
        currentLine.clear();
        currentWidth = 0;
        currentLineH = lineH;
        lineY += currentLineH;
        updateInsets();
        atLineStart = true;
    };

    // Available content width for the line currently being built.
    auto lineMaxW = [&](int forLineY) -> int {
        int left = floatLeftInset(floats, forLineY, lineH) - contentX;
        left = std::max(0, left);
        int rightEdge = floatRightEdge(floats, forLineY, lineH);
        int w = contentW - left;
        if (rightEdge < (1 << 20)) {
            int rightInset = std::max(0, contentX + contentW - rightEdge);
            w = std::max(20, contentW - left - rightInset);
        }
        return std::max(20, w - 8);
    };

    int mw = lineMaxW(lineY);
    if (getenv("MB_WRAPDBG"))
        std::fprintf(stderr, "[wrap] start contentX=%d contentW=%d startY=%d"
                             " lineH=%d floats=%zu mw=%d\n",
                     contentX, contentW, startY, lineH, floats.size(), mw);

    if (s.pre) {
        // white-space: pre — no wrapping, preserve \n and spaces.
        // (Float insets are ignored for pre lines; keep zeros.)
        curInsetL = curInsetR = 0;
        for (auto& run : runs) {
            if (run.isImage) {
                // Atomic image inside pre: put on its own line.
                if (!currentLine.empty()) {
                    out.lines.push_back(WrapLine{currentLine, currentLineH,
                                                 0, 0});
                    currentLine.clear(); currentWidth = 0;
                    lineY += currentLineH; currentLineH = lineH;
                }
                currentLine.push_back(run);
                currentWidth += run.drawW;
                currentLineH = std::max(currentLineH, run.drawH);
                out.lines.push_back(WrapLine{currentLine, currentLineH,
                                             0, 0});
                currentLine.clear(); currentWidth = 0;
                lineY += currentLineH; currentLineH = lineH;
                continue;
            }
            const std::string& t = run.text;
            size_t pos = 0;
            while (pos < t.size()) {
                size_t nl = t.find('\n', pos);
                std::string seg = (nl == std::string::npos)
                                    ? t.substr(pos)
                                    : t.substr(pos, nl - pos);
                if (!seg.empty()) {
                    Run nr = run;
                    nr.text = seg;
                    currentLine.push_back(nr);
                    int tw, th;
                    TTF_Font* rf = getFontForRun(run.text, nr.style.fontFamily,
                                                 nr.style.fontSize, nr.style.bold,
                                                 nr.style.italic, nullptr);
                    if (rf) { measureTextCached(rf, nr.text, tw, th); currentWidth += tw; }
                }
                if (nl == std::string::npos) break;
                out.lines.push_back(WrapLine{currentLine, currentLineH, curInsetL, curInsetR});
                currentLine.clear();
                currentWidth = 0;
                lineY += currentLineH;
                pos = nl + 1;
            }
        }
        if (!currentLine.empty()) out.lines.push_back(WrapLine{currentLine, currentLineH, curInsetL, curInsetR});
        return out;
    }

    for (auto& run : runs) {
        // ---- atomic inline image ----
        if (run.isImage) {
            int spaceW = 0;
            if (!atLineStart && !currentLine.empty()) {
                TTF_Font* rf0 = getFontForRun(run.text, run.style.fontFamily,
                                              run.style.fontSize, run.style.bold,
                                              run.style.italic, nullptr);
                int th;
                if (rf0) measureTextCached(rf0, " ", spaceW, th);
            }
            int imgW = run.drawW;
            int imgH = run.drawH;
            // Clamp to current line width.
            int effMw = lineMaxW(lineY);
            if (imgW > effMw) {
                int nh = imgH > 0 ? std::max(1, imgH * effMw / imgW) : imgH;
                imgW = effMw;
                imgH = nh;
            }
            if (!currentLine.empty() && currentWidth + spaceW + imgW > effMw) {
                out.lines.push_back(WrapLine{currentLine, currentLineH, curInsetL, curInsetR});
                currentLine.clear();
                currentWidth = 0;
                lineY += currentLineH;
                currentLineH = lineH;
                atLineStart = true;
                spaceW = 0;
            }
            Run r = run;
            r.drawW = imgW;
            r.drawH = imgH;
            if (!currentLine.empty() && spaceW) {
                // Preserve a single space before the image for spacing.
                Run sp;
                sp.text = " ";
                sp.style = run.style;
                sp.isLink = run.isLink;
                sp.href = run.href;
                sp.linkNode = run.linkNode;
                sp.sourceNode = run.sourceNode;
                currentLine.push_back(sp);
                currentWidth += spaceW;
            }
            currentLine.push_back(r);
            currentWidth += imgW;
            currentLineH = std::max(currentLineH, imgH + 2);
            atLineStart = false;
            continue;
        }

        // ---- text run ----
        // Font choice uses the WHOLE run text (CJK fallback switches the
        // entire run), matching the renderer exactly so wrapping matches.
        TTF_Font* rf = getFontForRun(run.text, run.style.fontFamily,
                                     run.style.fontSize, run.style.bold,
                                     run.style.italic, nullptr);
        std::string text = run.text;
        std::string word;
        atLineStart = atLineStart && currentLine.empty();

        for (size_t i = 0; i <= text.size(); ++i) {
            bool atEnd = (i == text.size());
            char c = atEnd ? ' ' : text[i];

            // "\n" run = <br> marker. Raw newlines in ordinary text are
            // pretty-printing whitespace — collapse them to spaces.
            if (c == '\n' && text != "\n") c = ' ';

            if (c == '\n') {
                flushLine();
                mw = lineMaxW(lineY);
                continue;
            }

            if (c == ' ' || c == '\t' || atEnd) {
                if (word.empty()) continue;

                int spaceW = 0, th = 0;
                if (!atLineStart) {
                    if (rf) measureTextCached(rf, " ", spaceW, th);
                }
                int tw = 0;
                if (rf) measureTextCached(rf, word, tw, th);
                mw = lineMaxW(lineY);

                if (currentWidth + spaceW + tw > mw && !currentLine.empty()) {
                    flushLine();
                    mw = lineMaxW(lineY);
                    spaceW = 0;
                }

                // Long-word breaking: hard-break words wider than the line.
                if (tw > mw) {
                    std::string remaining = word;
                    while (!remaining.empty()) {
                        std::string prefix;
                        int prefixW = 0;
                        for (size_t k = 0; k < remaining.size();) {
                            size_t step = 1;
                            unsigned char ch = (unsigned char)remaining[k];
                            if      (ch < 0x80)        step = 1;
                            else if ((ch >> 5) == 0x6)  step = 2;
                            else if ((ch >> 4) == 0xe)  step = 3;
                            else if ((ch >> 3) == 0x1e) step = 4;
                            step = std::min(step, remaining.size() - k);
                            std::string cp = remaining.substr(k, step);
                            int cw = 0, chh = 0;
                            if (rf) measureTextCached(rf, prefix + cp, cw, chh);
                            if (cw > mw && !prefix.empty()) break;
                            prefix += cp;
                            prefixW = cw;
                            k += step;
                        }
                        if (prefix.empty()) prefix = remaining.substr(0, 1);
                        if (!currentLine.empty()) {
                            out.lines.push_back(WrapLine{currentLine, currentLineH, curInsetL, curInsetR});
                            currentLine.clear();
                            currentWidth = 0;
                            lineY += currentLineH;
                            currentLineH = lineH;
                            atLineStart = true;
                        }
                        Run nr;
                        nr.text = prefix;
                        nr.style = run.style;
                        nr.isLink = run.isLink;
                        nr.href   = run.href;
                        nr.linkNode = run.linkNode;
                        nr.sourceNode = run.sourceNode;
                        currentLine.push_back(nr);
                        currentWidth += prefixW;
                        out.maxLineWidth = std::max(out.maxLineWidth, currentWidth);
                        atLineStart = false;
                        remaining = remaining.substr(prefix.size());
                    }
                    word.clear();
                    continue;
                }

                if (!currentLine.empty() &&
                    stylesMatch(currentLine.back().style, run.style) &&
                    !currentLine.back().isImage &&
                    // Only merge runs produced by the SAME element —
                    // otherwise the merged run's sourceNode would claim
                    // another element's fragment and its rect would
                    // over- (or under-) cover. Same-owner merging keeps
                    // inline-element fragment rects exact.
                    currentLine.back().sourceNode.lock() ==
                        run.sourceNode.lock()) {
                    currentLine.back().text += (atLineStart ? "" : " ") + word;
                    currentLine.back().isLink = currentLine.back().isLink || run.isLink;
                    if (run.isLink) currentLine.back().href = run.href;
                } else {
                    Run nr;
                    nr.text = (atLineStart ? "" : " ") + word;
                    nr.style = run.style;
                    nr.isLink = run.isLink;
                    nr.href   = run.href;
                    nr.linkNode = run.linkNode;
                    nr.sourceNode = run.sourceNode;
                    currentLine.push_back(nr);
                }
                currentWidth += spaceW + tw;
                atLineStart = false;
                word.clear();
            } else {
                word += c;
            }
        }
    }

    if (!currentLine.empty()) {
        out.lines.push_back(WrapLine{currentLine, currentLineH, curInsetL, curInsetR});
        out.maxLineWidth = std::max(out.maxLineWidth, currentWidth);
    }
    if (getenv("MB_WRAPDBG"))
        std::fprintf(stderr, "[wrap] done lines=%zu maxW=%d\n",
                     out.lines.size(), out.maxLineWidth);
    return out;
}

// ---------------------------------------------------------------------------
// Layout context helpers
// ---------------------------------------------------------------------------

// Shift a [begin, end) range of boxes and links by (dx, dy).
static void shiftRange(std::vector<Box>& boxes, std::vector<Link>& links,
                       size_t b0, size_t b1, size_t l0, size_t l1,
                       int dx, int dy) {
    for (size_t i = b0; i < b1 && i < boxes.size(); ++i) {
        boxes[i].x += dx;
        boxes[i].y += dy;
    }
    for (size_t i = l0; i < l1 && i < links.size(); ++i) {
        links[i].x += dx;
        links[i].y += dy;
    }
}

static std::string resolveResourceUrl(const std::string& u) {
    if (u.empty()) return u;
    if (isRemoteUrl(u)) return u;
    if (u.compare(0, 5, "data:") == 0) return u;
    return ResourceLoader::instance().resolve(u);
}

// PART2_MARKER
// ---------------------------------------------------------------------------

static void layoutBlockContainer(const std::shared_ptr<Node>& node,
                                 const Style& s, const Style& parentStyle,
                                 int x, int& y, int width,
                                 TTF_Font* measureFont,
                                 const std::vector<CSSRule>& cssRules,
                                 std::vector<Box>& boxes,
                                 std::vector<Link>& links,
                                 std::string linkHref,
                                 std::vector<FloatRegion>& floats);

static void layoutNode(const std::shared_ptr<Node>& node,
                       const Style& parentStyle,
                       int x, int& y, int width,
                       TTF_Font* measureFont,
                       const std::vector<CSSRule>& cssRules,
                       std::vector<Box>& boxes,
                       std::vector<Link>& links,
                       std::string linkHref,
                       std::vector<FloatRegion>& floats);

// Resolve the box's base text direction: "rtl"/"ltr" as declared,
// "auto" via first-strong detection over the given content text.
static bool resolveBaseRTL(const Style& s, const std::string& contentText) {
    if (s.direction == "rtl") return true;
    if (s.direction == "auto") return detectRTL(contentText);
    return false;
}

// Convert wrapped lines into a Box (fills lines, lineHeights, height).
// Also resolves the box's base text direction (CSS direction / dir attr /
// dir=auto first-strong detection) and reorders + shapes every line's
// runs into visual order via the bidi shaper.
static void fillBoxFromLines(Box& b, const std::vector<WrapLine>& wlines,
                             const Style& s) {
    std::string all;
    if (s.direction == "auto") {
        for (auto& wl : wlines)
            for (auto& r : wl.runs) {
                all += r.text;
                all += ' ';
            }
    }
    bool baseRTL = resolveBaseRTL(s, all);
    b.rtl = baseRTL;

    int totalH = 0;
    for (auto& wl : wlines) {
        b.lines.push_back(wl.runs);
        b.lineHeights.push_back(wl.height);
        b.lineInsets.push_back({wl.insetLeft, wl.insetRight});
        totalH += wl.height;
    }
    for (auto& line : b.lines) applyBidiToLine(line, baseRTL);
    b.h = std::max((int)s.fontSize + 4,
                   totalH + 4 + s.padding.vertical() + s.border.vertical());
    if ((s.declared & B_HEIGHT) && s.height >= 0) b.h = std::max(b.h, s.height);
    if ((s.declared & B_MIN_HEIGHT) && s.minHeight >= 0) b.h = std::max(b.h, s.minHeight);
    if ((s.declared & B_MAX_HEIGHT) && s.maxHeight >= 0) b.h = std::min(b.h, std::max(s.maxHeight, s.fontSize + 4));
}

// Emit link hit-rects for the runs in a laid-out box.
static void emitLinkRects(Box& b, const Style& s,
                          std::vector<Link>& links) {
    int lineY = b.y + s.padding.top + s.border.top + 2;
    for (size_t li = 0; li < b.lines.size(); ++li) {
        int curLineH = (li < b.lineHeights.size())
                     ? b.lineHeights[li]
                     : (s.hasLineHeight && s.lineHeight > 0
                          ? (int)std::lround(s.lineHeight) : s.fontSize + 6);
        int rx = b.x + s.padding.left + s.border.left + 4;
        // Match the renderer's per-line float inset so link hit-rects
        // sit exactly where the text is drawn.
        if (li < b.lineInsets.size())
            rx += std::max(0, b.lineInsets[li].first);
        for (auto& r : b.lines[li]) {
            if (r.isImage) {
                if (r.isLink && !r.href.empty()) {
                    Link lk;
                    lk.x = rx;
                    lk.y = lineY;
                    lk.w = std::max(1, r.drawW);
                    lk.h = std::max(curLineH, r.drawH);
                    lk.href = r.href;
                    lk.displayText = "(image link)";
                    lk.sourceNode = r.linkNode;
                    links.push_back(lk);
                }
                rx += r.drawW;
                continue;
            }
            if (r.text.empty()) continue;
            TTF_Font* rf2 = getFontForRun(r.logical.empty() ? r.text : r.logical,
                                          r.style.fontFamily, r.style.fontSize,
                                          r.style.bold, r.style.italic, nullptr);
            int tw = 0, th = 0;
            if (rf2) measureTextCached(rf2, r.text, tw, th);

            if (r.isLink && !r.href.empty()) {
                int startX = rx;
                int w = tw;
                if (!r.text.empty() && r.text[0] == ' ') {
                    int sp = 0, sh = 0;
                    if (rf2) measureTextCached(rf2, " ", sp, sh);
                    startX += sp;
                    w -= sp;
                }
                Link lk;
                lk.x = startX;
                lk.y = lineY;
                lk.w = std::max(1, w);
                lk.h = curLineH;
                lk.href = r.href;
                lk.displayText = r.logical.empty() ? r.text : r.logical;
                lk.sourceNode = r.linkNode;
                links.push_back(lk);
            }
            rx += tw;
        }
        lineY += curLineH;
    }
}

// Lay out an <img> as its own block-level box.
static void layoutImage(const std::shared_ptr<Node>& node,
                        const Style& parentStyle,
                        int x, int& y, int availW,
                        const std::vector<CSSRule>& cssRules,
                        std::vector<Box>& boxes) {
    Style s = computeStyle(parentStyle, node, cssRules);
    y += s.margin.top;

    Box b;
    b.isImage = true;
    b.sourceNode = node;
    b.style = s;

    std::string path, alt;
    int natW = 0, natH = 0;
    prepareImage(node, s, cssRules, availW, path, natW, natH,
                 b.drawW, b.drawH, alt);
    b.imagePath = path;
    b.imageW = natW;
    b.imageH = natH;
    b.altText = alt;
    b.x = x;
    b.y = y;
    b.w = b.drawW;
    b.h = b.drawH;

    boxes.push_back(b);
    y += b.h + 4 + s.margin.bottom;
}

// -------------------------------------------------------------------------
// Media elements (<video> / <audio>) — internal player widgets
// -------------------------------------------------------------------------

// Size + source resolution for the internal media player. Sizing follows
// the same precedence as images: CSS width/height, then the width/height
// attributes, then tag defaults (video 320x180 16:9, audio bar 360x36).
// Children of <video> (fallback text, <source>) are never laid out; the
// first <source src> is honored instead of a missing src attribute.
static void layoutMedia(const std::shared_ptr<Node>& node,
                        const Style& parentStyle,
                        int x, int& y, int availW,
                        const std::vector<CSSRule>& cssRules,
                        std::vector<Box>& boxes) {
    Style s = computeStyle(parentStyle, node, cssRules);
    y += s.margin.top;
    bool audio = (node->tag == "audio");

    std::string src;
    auto srcIt = node->attrs.find("src");
    if (srcIt != node->attrs.end() && !srcIt->second.empty())
        src = srcIt->second;
    if (src.empty()) {
        for (auto& c : node->children) {
            if (c->tag == "source") {
                auto sIt = c->attrs.find("src");
                if (sIt != c->attrs.end() && !sIt->second.empty()) {
                    src = sIt->second;
                    break;
                }
            }
        }
    }

    Box b;
    b.sourceNode = node;
    b.style = s;
    b.isAudio = audio;
    b.isVideo = !audio;
    // Resolve like images/stylesheets do (base dir / page URL / <base
    // href>) — the resolved string doubles as the media:: player
    // registry key, so layout, renderer and input all agree on identity.
    b.mediaPath = src.empty() ? std::string()
                              : ResourceLoader::instance().resolve(src);
    auto posterIt = node->attrs.find("poster");
    if (posterIt != node->attrs.end() && !posterIt->second.empty())
        b.posterPath = ResourceLoader::instance().resolve(posterIt->second);

    int cssW = -1, cssH = -1;
    if (s.hasWidth && s.width >= 0)
        cssW = s.width + (int)std::lround(s.widthPct * availW / 100.0f);
    if (s.hasHeight && s.height >= 0) cssH = s.height;
    int reqW = attrInt(node, "width"), reqH = attrInt(node, "height");

    int dw, dh;
    if (audio) {
        dw = cssW > 0 ? cssW : (reqW > 0 ? reqW
                              : std::min(availW > 0 ? availW : 360, 360));
        dh = cssH > 0 ? cssH : 36;
        if (availW > 0 && dw > availW) dw = availW;
        dw = std::max(120, dw);
        dh = std::max(24, dh);
    } else {
        if      (cssW > 0 && cssH > 0) { dw = cssW; dh = cssH; }
        else if (reqW > 0 && reqH > 0) { dw = reqW; dh = reqH; }
        else if (cssW > 0)             { dw = cssW; dh = cssW * 9 / 16; }
        else if (cssH > 0)             { dh = cssH; dw = dh * 16 / 9; }
        else if (reqW > 0)             { dw = reqW; dh = std::max(24, dw * 9 / 16); }
        else if (reqH > 0)             { dh = reqH; dw = dh * 16 / 9; }
        else                           { dw = 320;  dh = 180; }
        int maxW = availW > 0 ? availW : dw;
        if (dw > maxW && dw > 0) {
            dh = std::max(24, dh * maxW / dw);
            dw = maxW;
        }
        dw = std::max(40, dw);
        dh = std::max(24, dh);
    }
    b.x = x;
    b.y = y;
    b.w = dw;
    b.h = dh;
    b.drawW = dw;
    b.drawH = dh;
    boxes.push_back(b);
    y += b.h + 4 + s.margin.bottom;
}

// -------------------------------------------------------------------------
// Widgets (inputs, selects, textareas)
// -------------------------------------------------------------------------

static void layoutInput(const std::shared_ptr<Node>& node,
                        const Style& parentStyle,
                        int x, int& y, int width,
                        TTF_Font* measureFont,
                        const std::vector<CSSRule>& cssRules,
                        std::vector<Box>& boxes) {
    std::string type = node->attrs.count("type") ? node->attrs["type"] : "text";
    // Hidden inputs occupy no space and paint nothing (Google's search
    // form ships a dozen of them — they used to render as stray boxes).
    if (type == "hidden") return;
    Style s = computeStyle(parentStyle, node, cssRules);

    if (type == "checkbox" || type == "radio") {
        Box b;
        b.x = x; b.y = y; b.w = 14; b.h = 14;
        b.sourceNode = node;
        b.style = s;
        b.style.border.set1(1);
        b.style.hasBorder = true;
        b.style.borderColor = {80, 80, 80, 255};
        b.style.borderTopColor = b.style.borderRightColor =
            b.style.borderBottomColor = b.style.borderLeftColor = {80, 80, 80, 255};
        b.style.hasBorderColor = true;
        b.style.bg = {255, 255, 255, 255};
        b.style.hasBg = true;
        std::string checked = node->attrs.count("checked") ? "X" : "";
        if (!checked.empty()) {
            Run r;
            r.text = checked;
            r.style = s;
            b.lines.push_back({r});
        }
        boxes.push_back(b);
        y += b.h + 4;
        return;
    }

    if (type == "submit" || type == "button") {
        s.bg = SDL_Color{225, 230, 235, 255};
        s.hasBg = true;
        Box b;
        std::string label = node->attrs.count("value") ? node->attrs["value"] : "Submit";
        int labelW = 0, labelH = 0;
        // Run-level font choice (family/CJK fallback + real fallback when
        // the style is default): a null measure font made labels measure
        // 0px and every submit button painted empty.
        TTF_Font* rf = getFontForRun(label, s.fontFamily, s.fontSize,
                                     s.bold, s.italic, measureFont);
        if (rf) measureTextCached(rf, label, labelW, labelH);
        b.x = x; b.y = y;
        b.w = std::max(60, labelW + 16);
        b.h = 22;
        b.sourceNode = node;
        b.style = s;
        b.style.border.set1(1);
        b.style.hasBorder = true;
        b.style.padding.set1(2);
        b.style.hasPadding = true;
        Run r;
        r.text = label;
        r.style = s;
        b.lines.push_back({r});
        boxes.push_back(b);
        y += b.h + 4;
        return;
    }

    // Default: text-ish field.
    Box b;
    b.x = x; b.y = y;
    if ((s.declared & B_WIDTH) && s.width > 0) b.w = s.width;
    else b.w = std::max(120, std::min(width, 200));
    b.h = 24;
    b.sourceNode = node;
    b.style = s;
    b.style.border.set1(1);
    b.style.hasBorder = true;
    b.style.borderColor = {120, 120, 120, 255};
    b.style.borderTopColor = b.style.borderRightColor =
        b.style.borderBottomColor = b.style.borderLeftColor = {120, 120, 120, 255};
    b.style.hasBorderColor = true;
    b.style.bg = {255, 255, 255, 255};
    b.style.hasBg = true;
    b.style.padding.set1(3);
    b.style.hasPadding = true;
    std::string val = node->attrs.count("value") ? node->attrs["value"] : "";
    if (type == "password") {
        std::string masked;
        for (size_t i = 0; i < val.size(); ++i) masked += '*';
        val = masked;
    }
    Run r;
    r.text = val;
    r.style = s;
    r.sourceNode = node;   // the <input> itself
    b.rtl = resolveBaseRTL(s, val);
    b.lines.push_back({r});
    applyBidiToLine(b.lines.back(), b.rtl);
    boxes.push_back(b);
    y += b.h + 4;
}

static void layoutSelect(const std::shared_ptr<Node>& node,
                         const Style& parentStyle,
                         int x, int& y, int width,
                         const std::vector<CSSRule>& cssRules,
                         std::vector<Box>& boxes) {
    Style s = computeStyle(parentStyle, node, cssRules);
    Box b;
    b.x = x; b.y = y;
    if ((s.declared & B_WIDTH) && s.width > 0) b.w = s.width;
    else b.w = std::max(80, std::min(width, 160));
    b.h = 24;
    b.sourceNode = node;
    b.style = s;
    b.style.border.set1(1);
    b.style.hasBorder = true;
    b.style.borderColor = {120, 120, 120, 255};
    b.style.borderTopColor = b.style.borderRightColor =
        b.style.borderBottomColor = b.style.borderLeftColor = {120, 120, 120, 255};
    b.style.hasBorderColor = true;
    b.style.bg = {255, 255, 255, 255};
    b.style.hasBg = true;
    b.style.padding.set1(3);
    b.style.hasPadding = true;
    std::string selectedText;
    bool foundSelected = false;
    for (auto& c : node->children) {
        if (c->tag == "option") {
            bool isSelected = c->attrs.count("selected") > 0;
            std::string text;
            for (auto& tc : c->children)
                if (tc->tag == "text") text += tc->text;
            if (isSelected || (!foundSelected && selectedText.empty())) {
                selectedText = text;
                if (isSelected) foundSelected = true;
            }
        }
    }
    selectedText += " v";
    Run r;
    r.text = selectedText;
    r.style = s;
    r.sourceNode = node;   // the <select> itself
    b.rtl = resolveBaseRTL(s, selectedText);
    b.lines.push_back({r});
    applyBidiToLine(b.lines.back(), b.rtl);
    boxes.push_back(b);
    y += b.h + 4;
}

static void layoutTextarea(const std::shared_ptr<Node>& node,
                           const Style& parentStyle,
                           int x, int& y,
                           const std::vector<CSSRule>& cssRules,
                           std::vector<Box>& boxes) {
    Style s = computeStyle(parentStyle, node, cssRules);
    Box b;
    int cols = 30, rows = 4;
    if (node->attrs.count("cols")) {
        try { cols = std::stoi(node->attrs["cols"]); } catch (...) {}
    }
    if (node->attrs.count("rows")) {
        try { rows = std::stoi(node->attrs["rows"]); } catch (...) {}
    }
    b.x = x; b.y = y;
    b.w = std::max(80, cols * 8);
    b.h = std::max(20, rows * 18);
    b.sourceNode = node;
    b.style = s;
    b.style.border.set1(1);
    b.style.hasBorder = true;
    b.style.borderColor = {120, 120, 120, 255};
    b.style.borderTopColor = b.style.borderRightColor =
        b.style.borderBottomColor = b.style.borderLeftColor = {120, 120, 120, 255};
    b.style.hasBorderColor = true;
    b.style.bg = {255, 255, 255, 255};
    b.style.hasBg = true;
    b.style.padding.set1(3);
    b.style.hasPadding = true;
    std::string val;
    for (auto& c : node->children)
        if (c->tag == "text") val += c->text;
    std::vector<Run> currentLine;
    std::string word;
    for (size_t i = 0; i <= val.size(); ++i) {
        bool atEnd = (i == val.size());
        char c = atEnd ? '\n' : val[i];
        if (c == '\n') {
            if (!word.empty() || !currentLine.empty()) {
                Run r;
                r.text = word;
                r.style = s;
                r.sourceNode = node;   // the <textarea> itself
                currentLine.push_back(r);
                b.lines.push_back(currentLine);
                b.lineHeights.push_back(s.fontSize + 6);
                currentLine.clear();
                word.clear();
            }
        } else {
            word += c;
        }
    }
    if (!currentLine.empty()) {
        b.lines.push_back(currentLine);
        b.lineHeights.push_back(s.fontSize + 6);
    }
    b.rtl = resolveBaseRTL(s, val);
    for (auto& line : b.lines) applyBidiToLine(line, b.rtl);
    boxes.push_back(b);
    y += b.h + 4;
}

// -------------------------------------------------------------------------
// Tables (with colspan support)
// -------------------------------------------------------------------------

// Measure a table's natural column widths from its cells' content.
// Nested <table> descendants count toward their cell's demand (block
// content inside cells otherwise measures as zero, which crushed
// nested-table layouts like Hacker News to the 40px minimum).
static std::vector<int> measureTableColumns(const std::shared_ptr<Node>& node,
                                            const Style& s,
                                            const std::vector<CSSRule>& cssRules,
                                            TTF_Font* measureFont,
                                            const std::string& linkHref,
                                            int availW,
                                            int depth = 0);

// Sum of the natural column widths of every <table> under `node`.
static int nestedTablesDemand(const std::shared_ptr<Node>& node,
                              const Style& s,
                              const std::vector<CSSRule>& cssRules,
                              TTF_Font* measureFont,
                              const std::string& linkHref,
                              int availW, int depth) {
    if (depth <= 0) return 0;
    int best = 0;
    for (auto& c : node->children) {
        if (c->tag == "table") {
            std::vector<int> cw = measureTableColumns(c, s, cssRules,
                                                      measureFont, linkHref,
                                                      availW, depth - 1);
            int tot = 0;
            for (int w : cw) tot += w;
            best = std::max(best, tot);
        } else {
            best = std::max(best, nestedTablesDemand(c, s, cssRules,
                                                     measureFont, linkHref,
                                                     availW, depth));
        }
    }
    return best;
}

static std::vector<int> measureTableColumns(const std::shared_ptr<Node>& node,
                                            const Style& s,
                                            const std::vector<CSSRule>& cssRules,
                                            TTF_Font* measureFont,
                                            const std::string& linkHref,
                                            int availW,
                                            int depth) {
    // Collect rows: each entry is a list of cells plus per-cell colspan.
    struct Cell { std::shared_ptr<Node> n; int span; };
    std::vector<std::vector<Cell>> rows;
    auto collectRow = [&](const std::shared_ptr<Node>& tr) {
        std::vector<Cell> tds;
        for (auto& tc : tr->children) {
            if (tc->tag == "td" || tc->tag == "th") {
                int span = attrInt(tc, "colspan");
                if (span < 1) span = 1;
                tds.push_back({tc, span});
            }
        }
        if (!tds.empty()) rows.push_back(tds);
    };
    for (auto& c : node->children) {
        if (c->tag == "tr") collectRow(c);
        else if (c->tag == "thead" || c->tag == "tbody" || c->tag == "tfoot")
            for (auto& tr : c->children)
                if (tr->tag == "tr") collectRow(tr);
    }
    if (rows.empty()) return {};

    // Column count (colspan-aware).
    size_t ncol = 0;
    for (auto& r : rows) {
        size_t n = 0;
        for (auto& c : r) n += c.span;
        ncol = std::max(ncol, n);
    }
    if (ncol == 0) return {};

    // Measure natural width of each cell's content; single-span cells
    // drive their column, spanning cells distribute demand evenly.
    std::vector<int> colW(ncol, 40);
    for (auto& r : rows) {
        size_t col = 0;
        for (auto& cell : r) {
            std::vector<Run> runs;
            Style cs = computeStyle(s, cell.n, cssRules);
            collectRuns(cell.n, cs, false, false, linkHref,
                        std::weak_ptr<Node>{}, cssRules, availW, runs);
            int w = 0;
            for (auto& run : runs) {
                if (run.isImage) { w += run.drawW + 2; continue; }
                TTF_Font* rf = getFontForRun(run.text, run.style.fontFamily,
                                             run.style.fontSize, run.style.bold,
                                             run.style.italic, measureFont);
                int tw = 0, th = 0;
                if (rf) measureTextCached(rf, run.text, tw, th);
                w += tw + 2;
            }
            w += 12 + cs.padding.horizontal() + cs.border.horizontal();
            w = std::max(w, (cs.declared & B_WIDTH) && cs.width > 0 ? cs.width : 0);
            if (depth > 0)
                w = std::max(w, nestedTablesDemand(cell.n, cs, cssRules,
                                                   measureFont, linkHref,
                                                   availW, depth - 1) + 4);
            int demand = w / (int)cell.span + 1;
            for (int k = 0; k < cell.span && col + k < ncol; ++k)
                colW[col + k] = std::max(colW[col + k], demand);
            col += cell.span;
        }
    }
    return colW;
}

static void layoutTable(const std::shared_ptr<Node>& node,
                        const Style& s,
                        int x, int& y, int width,
                        TTF_Font* measureFont,
                        const std::vector<CSSRule>& cssRules,
                        std::vector<Box>& boxes,
                        std::vector<Link>& links,
                        std::string linkHref,
                        std::vector<FloatRegion>& floats) {
    y += s.margin.top;
    // Column widths: shared measurement (also counts nested tables).
    std::vector<int> colW = measureTableColumns(node, s, cssRules, measureFont,
                                                linkHref, width, 4);
    if (colW.empty()) { y += s.margin.bottom; return; }
    size_t ncol = colW.size();

    // Shrink columns if total exceeds available width.
    int total = 0;
    for (auto cw : colW) total += cw;
    if (total > width) {
        for (auto& cw : colW) cw = std::max(20, cw * width / std::max(1, total));
    }

    // HTML width attribute on the table ("85%", "600") acts as a MINIMUM
    // table width — extra space is distributed across the columns
    // proportionally. Nested-table layouts (Hacker News) depend on this:
    // the outer table sizes the cell, the inner width=100% table fills it.
    {
        auto wit = node->attrs.find("width");
        if (wit != node->attrs.end() && !wit->second.empty()) {
            std::string v = wit->second;
            int attrW = 0;
            if (v.back() == '%') {
                try { attrW = (int)std::lround(std::stof(v) * width / 100.0f); }
                catch (...) { attrW = 0; }
            } else {
                try { attrW = (int)std::lround(std::stof(v)); } catch (...) {}
            }
            if (total > 0) attrW = std::min(attrW, width);
            if (attrW > total) {
                int extra = attrW - total;
                for (auto& cw : colW) cw += (int)((long long)cw * extra / total);
                total = attrW;
            }
        }
    }

    bool attrBorder = attrInt(node, "border") > 0;
    // Re-collect rows for placement (measurement uses its own copy).
    struct Cell { std::shared_ptr<Node> n; int span; };
    std::vector<std::vector<Cell>> rows;
    {
        auto collectRow = [&](const std::shared_ptr<Node>& tr) {
            std::vector<Cell> tds;
            for (auto& tc : tr->children) {
                if (tc->tag == "td" || tc->tag == "th") {
                    int span = attrInt(tc, "colspan");
                    if (span < 1) span = 1;
                    tds.push_back({tc, span});
                }
            }
            if (!tds.empty()) rows.push_back(tds);
        };
        for (auto& c : node->children) {
            if (c->tag == "tr") collectRow(c);
            else if (c->tag == "thead" || c->tag == "tbody" || c->tag == "tfoot")
                for (auto& tr : c->children)
                    if (tr->tag == "tr") collectRow(tr);
        }
        if (rows.empty()) { y += s.margin.bottom; return; }
    }
    // Cell frame for border="1" — injected via the style overlay (the
    // box model no longer inherits, so pre-setting cs.border before
    // layoutNode would be silently reset).
    Style cellOverlay;
    if (attrBorder) {
        cellOverlay.border.set1(1);
        cellOverlay.hasBorder = true;
        cellOverlay.borderColor = cellOverlay.borderTopColor =
            cellOverlay.borderRightColor = cellOverlay.borderBottomColor =
            cellOverlay.borderLeftColor = {150, 150, 150, 255};
        cellOverlay.hasBorderColor = true;
        cellOverlay.declared |= B_BORDER | B_BORDER_COLOR;
    }

    for (auto& r : rows) {
        int rowY = y;
        int cx = x;
        int rowH = 0;
        size_t col = 0;
        for (auto& cell : r) {
            int cw = 0;
            for (int k = 0; k < cell.span && col + k < ncol; ++k) cw += colW[col + k];
            int cellY = rowY;
            Style cs = computeStyle(s, cell.n, cssRules);
            if (cell.n->tag == "th") { cs.bold = true; cs.hasBold = true; }
            g_styleOverlay = attrBorder ? &cellOverlay : nullptr;
            layoutNode(cell.n, cs, cx, cellY, cw, measureFont, cssRules,
                       boxes, links, linkHref, floats);
            g_styleOverlay = nullptr;
            rowH = std::max(rowH, cellY - rowY);
            cx += cw;
            col += cell.span;
        }
        y = rowY + rowH;
    }
    y += s.margin.bottom;
}

// PART3_MARKER
// -------------------------------------------------------------------------
// Flex layout (simplified: row/column, gap, justify-content, align-items)
// -------------------------------------------------------------------------

// Measure the natural width of a laid-out box range (max line width).
static int measureRangeNaturalW(const std::vector<Box>& boxes,
                                size_t b0, size_t b1) {
    int w = 0;
    for (size_t i = b0; i < b1 && i < boxes.size(); ++i) {
        const Box& b = boxes[i];
        if (b.isImage) { w = std::max(w, b.drawW); continue; }
        for (auto& line : b.lines) {
            int lw = 0;
            for (auto& r : line) {
                if (r.isImage) { lw += r.drawW; continue; }
                TTF_Font* rf = getFontFor(r.style, nullptr);
                if (!rf) rf = getFontForFamily(r.style.fontFamily, r.style.fontSize,
                                               r.style.bold, r.style.italic);
                int tw = 0, th = 0;
                if (rf) measureTextCached(rf, r.text, tw, th);
                lw += tw;
            }
            w = std::max(w, lw + b.style.padding.horizontal() +
                                b.style.border.horizontal() + 8);
        }
        if (b.lines.empty()) w = std::max(w, b.w);
    }
    return w;
}

// ---------------------------------------------------------------------------
// Grid containers (basic CSS Grid)
//
// Supports the constructs real sites actually use:
//   display: grid / inline-grid
//   grid-template-columns / grid-template-rows / grid-template: r / c
//   with px, %, rem/em/pt, Nfr, auto/min-content/max-content,
//   minmax(a,b) and repeat(N | auto-fill | auto-fit, ...)
//   grid-template-areas + grid-area: <name> placement, row-major
//   auto-placement for children without a named area, gap/column-gap/row-gap
// ---------------------------------------------------------------------------

struct GridTrack {
    float px = 0;    // fixed part
    float pct = 0;   // % of container content width
    float fr = 0;    // flexible share
};

static std::string gridTrim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// Parse a single track token (no repeat()): "200px", "50%", "1fr",
// "auto", "min-content", "max-content", "minmax(0,1fr)".
static GridTrack parseOneTrack(const std::string& tok, int fontSize) {
    GridTrack t;
    std::string s = tok;
    for (auto& ch : s) ch = (char)tolower((unsigned char)ch);
    s = gridTrim(s);
    if (s.rfind("minmax(", 0) == 0 && s.back() == ')') {
        std::string inner = s.substr(7, s.size() - 8);
        size_t comma = inner.find(',');
        std::string mn = comma == std::string::npos ? inner
                                                    : inner.substr(0, comma);
        std::string mx = comma == std::string::npos ? ""
                                                    : inner.substr(comma + 1);
        // min side: fixed if px/%/rem..., else 0.
        float px = 0, pct = 0;
        std::string mnc = gridTrim(mn);
        if (mnc != "auto" && mnc != "min-content" && mnc != "max-content") {
            // reuse simple length parse
            size_t i = 0;
            while (i < mnc.size() && (isdigit((unsigned char)mnc[i]) ||
                                      mnc[i] == '.' || mnc[i] == '-' ||
                                      mnc[i] == '+')) ++i;
            float n = 0;
            try { n = std::stof(mnc.substr(0, i)); } catch (...) { n = 0; }
            std::string unit = mnc.substr(i);
            if (unit == "px" || unit.empty()) px = n;
            else if (unit == "rem" || unit == "em") px = n * fontSize;
            else if (unit == "pt") px = n * 96.0f / 72.0f;
            else if (unit == "%") pct = n;
        }
        t.px = px; t.pct = pct;
        std::string mxc = gridTrim(mx);
        if (!mxc.empty() && mxc.size() > 2 &&
            mxc.compare(mxc.size() - 2, 2, "fr") == 0) {
            try { t.fr = std::stof(mxc.substr(0, mxc.size() - 2)); }
            catch (...) { t.fr = 1; }
        } else if (mxc == "auto" || mxc == "min-content" || mxc == "max-content") {
            t.fr = 1;
        } else {
            // Fixed max ("59.25rem", "300px", "50%"): the track is that
            // fixed size (min < max, so max governs for sizing).
            size_t i = 0;
            while (i < mxc.size() && (isdigit((unsigned char)mxc[i]) ||
                                      mxc[i] == '.' || mxc[i] == '-' ||
                                      mxc[i] == '+')) ++i;
            float n = 0;
            try { n = std::stof(mxc.substr(0, i)); } catch (...) { n = 0; }
            std::string unit = mxc.substr(i);
            if (unit == "px" || unit.empty()) t.px = n;
            else if (unit == "rem" || unit == "em") t.px = n * fontSize;
            else if (unit == "pt") t.px = n * 96.0f / 72.0f;
            else if (unit == "%") t.pct = n;
        }
        return t;
    }
    if (s == "auto" || s == "min-content" || s == "max-content") {
        t.fr = 1;
        return t;
    }
    if (s.size() > 2 && s.compare(s.size() - 2, 2, "fr") == 0) {
        try { t.fr = std::stof(s.substr(0, s.size() - 2)); }
        catch (...) { t.fr = 1; }
        return t;
    }
    // Length with unit.
    size_t i = 0;
    while (i < s.size() && (isdigit((unsigned char)s[i]) || s[i] == '.' ||
                            s[i] == '-' || s[i] == '+')) ++i;
    float n = 0;
    try { n = std::stof(s.substr(0, i)); } catch (...) { return t; }
    std::string unit = s.substr(i);
    if (unit == "px" || unit.empty()) t.px = n;
    else if (unit == "rem" || unit == "em") t.px = n * fontSize;
    else if (unit == "pt") t.px = n * 96.0f / 72.0f;
    else if (unit == "%") t.pct = n;
    return t;
}

// Split `val` into top-level tokens (spaces outside parens).
static std::vector<std::string> gridTopTokens(const std::string& val) {
    std::vector<std::string> out;
    int depth = 0;
    std::string cur;
    for (char c : val) {
        if (c == '(') ++depth;
        else if (c == ')') --depth;
        if ((c == ' ' || c == '\t' || c == '\n' || c == '\r') && depth == 0) {
            if (!cur.empty()) { out.push_back(cur); cur.clear(); }
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

static std::vector<GridTrack> parseGridTracks(const std::string& val,
                                              int availW, int gap,
                                              int fontSize) {
    std::vector<GridTrack> out;
    for (const std::string& tok : gridTopTokens(val)) {
        if (tok.rfind("repeat(", 0) != 0 || tok.back() != ')') {
            out.push_back(parseOneTrack(tok, fontSize));
            continue;
        }
        std::string inner = tok.substr(7, tok.size() - 8);
        // First argument: count / auto-fill / auto-fit.
        size_t comma = inner.find(',');
        if (comma == std::string::npos) continue;
        std::string count = gridTrim(inner.substr(0, comma));
        std::string body = gridTrim(inner.substr(comma + 1));
        // Parse the inner track list (supports multi-track bodies).
        std::vector<GridTrack> one;
        for (const std::string& t : gridTopTokens(body))
            one.push_back(parseOneTrack(t, fontSize));
        if (one.empty()) continue;
        if (count == "auto-fill" || count == "auto-fit") {
            // Need the min size of one repetition to compute the count.
            float minW = 0;
            for (auto& t : one) minW += t.px + t.pct * availW / 100.0f;
            int n = 1;
            if (minW > 0)
                n = (int)((availW + gap) / (minW + gap));
            if (n < 1) n = 1;
            if (n > 48) n = 48;   // sanity cap
            for (int k = 0; k < n; ++k)
                out.insert(out.end(), one.begin(), one.end());
        } else {
            int n = 0;
            try { n = std::stoi(count); } catch (...) { n = 0; }
            if (n < 1) n = 1;
            if (n > 48) n = 48;
            for (int k = 0; k < n; ++k)
                out.insert(out.end(), one.begin(), one.end());
        }
    }
    return out;
}

// grid-template-areas: "'a b' 'c d'" → rows of area names.
static std::vector<std::vector<std::string>> parseGridAreas(
        const std::string& val) {
    std::vector<std::vector<std::string>> rows;
    std::vector<std::string> cur;
    bool inQuote = false;
    char q = 0;
    std::string tok;
    auto flush = [&]() {
        if (!tok.empty()) { cur.push_back(tok); tok.clear(); }
    };
    for (char c : val) {
        if (inQuote) {
            if (c == q) { inQuote = false; flush(); rows.push_back(cur); cur.clear(); }
            else if (c == ' ' || c == '\t') flush();
            else tok += c;
        } else if (c == '\'' || c == '"') {
            inQuote = true; q = c;
        }
    }
    return rows;
}

static void layoutGridChildren(const std::shared_ptr<Node>& node,
                               const Style& s,
                               int contentX, int& y, int contentW,
                               TTF_Font* measureFont,
                               const std::vector<CSSRule>& cssRules,
                               std::vector<Box>& boxes,
                               std::vector<Link>& links,
                               std::string linkHref,
                               std::vector<FloatRegion>& floats) {
    int colGap = s.flexGap, rowGap = s.flexGap;
    int fontSize = s.fontSize;

    // Element children (skip head-only tags, display:none, ws text).
    struct GChild { std::shared_ptr<Node> n; Style st; };
    std::vector<GChild> kids;
    for (auto& c : node->children) {
        if (kNoRender.count(c->tag)) continue;
        if (c->tag == "text") {
            bool wsOnly = true;
            for (char ch : c->text)
                if (!std::isspace((unsigned char)ch)) { wsOnly = false; break; }
            if (wsOnly) continue;
        }
        Style cs = computeStyle(s, c, cssRules);
        if (cs.hasDisplay && cs.display == "none") continue;
        // Out-of-flow children are not grid items; the container's own
        // absolute pass places them after the grid is laid out.
        if (cs.position == "absolute" || cs.position == "fixed") continue;
        kids.push_back(GChild{c, cs});
    }
    if (getenv("MB_GRIDDBG")) fprintf(stderr, "[grid] kids=%zu tpl='%s'\n", kids.size(), s.gridTemplateColumns.c_str());
    if (kids.empty()) return;

    // Column tracks.
    std::vector<GridTrack> cols;
    if (s.hasGridTemplateColumns && !s.gridTemplateColumns.empty())
        cols = parseGridTracks(s.gridTemplateColumns, contentW, colGap,
                               fontSize);
    if (cols.empty()) cols.push_back(GridTrack{0, 0, 1});  // single column

    // Resolve column widths against contentW.
    int ncol = (int)cols.size();
    std::vector<int> colW(ncol, 0);
    {
        float fixed = 0;
        float frTotal = 0;
        for (auto& t : cols) {
            fixed += t.px + t.pct * contentW / 100.0f;
            frTotal += t.fr;
        }
        float remaining = contentW - fixed - colGap * (float)(ncol - 1);
        if (remaining < 0) remaining = 0;
        for (int i = 0; i < ncol; ++i) {
            int w = (int)std::lround(cols[i].px + cols[i].pct * contentW / 100.0f);
            if (frTotal > 0)
                w += (int)std::lround(remaining * (cols[i].fr / frTotal));
            colW[i] = std::max(1, w);
        }
    }

    // Occupancy + named-area map (shared with resolvePlace below).
    auto areaRows = parseGridAreas(s.gridTemplateAreas);
    std::vector<std::vector<int>> occ;   // 0 free, 1 taken (lazy rows)
    std::map<std::string, std::pair<int,int>> areaPos;
    for (auto& row : areaRows) {
        std::vector<int> orow(std::max(ncol, (int)row.size()), 0);
        for (size_t c = 0; c < row.size(); ++c)
            if (!row[c].empty() && row[c] != ".")
                areaPos[row[c]] = {(int)occ.size(), (int)c};
        occ.push_back(orow);
    }
    int autoR = 0, autoC = 0;

    // Resolve placement for one child: explicit lines/spans from
    // grid-column / grid-row / numeric grid-area, else auto-placement
    // into the first free run of cells (skipping occupied cells and
    // clamping spans to the track count). Returns start row/col and the
    // span counts (>= 1).
    auto resolvePlace = [&](const Style& cst, int& r0, int& c0,
                            int& colSpanOut, int& rowSpanOut) {
        int colSpan = std::max(1, cst.gridColSpan);
        int rowSpan = std::max(1, cst.gridRowSpan);
        bool colExplicit = cst.hasGridColumn &&
                           (cst.gridColStartLine > 0 ||
                            cst.gridColEndLine != 0 || cst.gridColSpan > 0);
        bool rowExplicit = cst.hasGridRow &&
                           (cst.gridRowStartLine > 0 ||
                            cst.gridRowEndLine != 0 || cst.gridRowSpan > 0);
        // Column span from end line (supports negative from-the-end).
        if (cst.hasGridColumn && cst.gridColEndLine != 0) {
            int end = cst.gridColEndLine > 0
                          ? cst.gridColEndLine
                          : ncol + 1 + cst.gridColEndLine;   // -1 → last line
            int start = cst.gridColStartLine > 0 ? cst.gridColStartLine : 1;
            colSpan = std::max(1, end - start);
        }
        if (colSpan > ncol) colSpan = ncol;

        // Rows: track occupancy rows grow lazily.
        auto rowsAt = [&](int r) -> std::vector<int>& {
            while ((int)occ.size() <= r) occ.push_back(std::vector<int>(ncol, 0));
            return occ[r];
        };
        auto runFree = [&](int r, int c, int cs, int rs) {
            for (int rr = r; rr < r + rs; ++rr) {
                auto& row = rowsAt(rr);
                for (int cc = c; cc < c + cs; ++cc)
                    if (row[cc]) return false;
            }
            return true;
        };
        auto markRun = [&](int r, int c, int cs, int rs) {
            for (int rr = r; rr < r + rs; ++rr) {
                auto& row = rowsAt(rr);
                for (int cc = c; cc < c + cs; ++cc) row[cc] = 1;
            }
        };

        // Named grid-area placement still wins when it applies.
        if (cst.hasGridArea && !cst.gridArea.empty() && cst.gridArea != "auto") {
            auto it = areaPos.find(cst.gridArea);
            if (it != areaPos.end()) {
                r0 = it->second.first;
                c0 = it->second.second;
                colSpanOut = std::min(colSpan, ncol - c0);
                rowSpanOut = rowSpan;
                markRun(r0, c0, std::max(1, colSpanOut), rowSpanOut);
                return;
            }
        }

        int rs = std::max(1, rowSpan);
        if (rowExplicit && cst.gridRowStartLine > 0) {
            r0 = cst.gridRowStartLine - 1;
            if (cst.gridRowEndLine != 0) {
                int end = cst.gridRowEndLine > 0
                              ? cst.gridRowEndLine
                              : std::max(r0 + 1, 8 + 1 + cst.gridRowEndLine);
                rs = std::max(1, end - cst.gridRowStartLine);
            }
        }
        if (colExplicit && cst.gridColStartLine > 0) {
            c0 = std::min(ncol - colSpan, cst.gridColStartLine - 1);
            if (c0 < 0) c0 = 0;
        } else if (colExplicit && cst.gridColStartLine <= 0 &&
                   cst.gridColEndLine != 0) {
            // "span N" end-line form without a start: auto-place start.
            c0 = -1;
        } else {
            c0 = -1;   // fully auto column
        }
        if (!rowExplicit) r0 = -1;   // auto row

        if (c0 >= 0 && r0 >= 0) {
            // Fully explicit: claim (may overlap others, as in CSS).
            colSpanOut = std::min(colSpan, ncol - c0);
            rowSpanOut = rs;
            markRun(r0, c0, std::max(1, colSpanOut), rowSpanOut);
            return;
        }
        // Auto-place: scan (row-major) for the first free run.
        int r = std::max(0, r0), cStart = (r0 >= 0) ? 0 : autoC;
        int rr = r;
        bool found = false;
        for (; !found; ++rr) {
            for (int cc = cStart; cc + colSpan <= ncol; ++cc) {
                if (runFree(rr, cc, colSpan, rs)) {
                    r0 = rr; c0 = cc; found = true; break;
                }
            }
            cStart = 0;
            if (rr > 4000) break;   // paranoia
        }
        colSpanOut = std::min(colSpan, ncol - c0);
        rowSpanOut = rs;
        markRun(r0, c0, std::max(1, colSpanOut), rowSpanOut);
        // Advance the auto cursor past this placement.
        autoC = c0 + colSpanOut;
        autoR = r0;
        if (autoC >= ncol) { autoC = 0; ++autoR; }
    };

    struct Place { int row, col, colSpan, rowSpan; };
    std::vector<Place> places(kids.size(), Place{0, 0, 1, 1});
    for (size_t k = 0; k < kids.size(); ++k) {
        int r0, c0, cs, rs;
        resolvePlace(kids[k].st, r0, c0, cs, rs);
        places[k] = Place{r0, c0, cs, rs};
        if (getenv("MB_GRIDDBG"))
            fprintf(stderr, "[grid] kid %zu -> row=%d col=%d colSpan=%d"
                            " rowSpan=%d\n", k, r0, c0, cs, rs);
    }
    size_t rowsUsed = occ.size();
    if (rowsUsed == 0) rowsUsed = 1;

    // Lay out row by row. Row height = tallest rowSpan==1 child; children
    // spanning several rows are laid out at their start row and any
    // overflow beyond the covered rows extends the LAST covered row.
    int yRow = y;
    std::vector<int> rowH(rowsUsed, 0);
    std::vector<std::vector<std::pair<int,int>>> pendingAt(rowsUsed + 64);
    for (size_t r = 0; r < rowsUsed; ++r) {
        int rowMaxH = 0;
        int y0 = yRow;
        for (size_t k = 0; k < kids.size(); ++k) {
            if (places[k].row != (int)r) continue;
            int c = places[k].col;
            int cs = std::max(1, std::min(places[k].colSpan,
                                          ncol - c));
            int cx = contentX;
            for (int i = 0; i < c; ++i) cx += colW[i] + colGap;
            int cy = y0;
            int w = 0;
            for (int i = c; i < c + cs && i < ncol; ++i) w += colW[i];
            w += colGap * (cs - 1);
            if (w < 1) w = 1;
            // Blocks in grid stretch to the cell by default; margins
            // still apply inside layoutNode.
            size_t bb0 = boxes.size();
            layoutNode(kids[k].n, s, cx, cy, w, measureFont, cssRules,
                       boxes, links, linkHref, floats);
            if (getenv("MB_GRIDDBG")) fprintf(stderr, "[grid] kid %zu at (%d,%d) w=%d -> %zu boxes, dh=%d\n", k, cx, y0, w, boxes.size()-bb0, cy-y0);
            int h = cy - y0;
            if (places[k].rowSpan <= 1) {
                rowMaxH = std::max(rowMaxH, h);
            } else {
                size_t last = std::min((size_t)(r + places[k].rowSpan - 1),
                                       rowsUsed - 1);
                if (last > r) pendingAt[last].push_back({(int)r, h});
                else rowMaxH = std::max(rowMaxH, h);
            }
        }
        rowMaxH += 0;
        // Children whose multi-row span ends at this row may need the
        // covered rows to grow to fit their content.
        for (auto& [r0c, h] : pendingAt[r]) {
            int covered = rowGap * (int)r - rowGap * r0c;   // gaps only
            for (int rr = r0c; rr <= (int)r; ++rr)
                covered += (rr == (int)r) ? rowMaxH : rowH[rr];
            if (h > covered) rowMaxH += h - covered;
        }
        rowH[r] = std::max(1, rowMaxH);
        yRow = y0 + rowH[r] + rowGap;
    }
    if (rowsUsed > 0) yRow -= rowGap;   // no trailing gap
    y = yRow;
}

static void layoutFlexChildren(const std::shared_ptr<Node>& node,
                               const Style& s,
                               int contentX, int& y, int contentW,
                               TTF_Font* measureFont,
                               const std::vector<CSSRule>& cssRules,
                               std::vector<Box>& boxes,
                               std::vector<Link>& links,
                               std::string linkHref,
                               std::vector<FloatRegion>& floats) {
    bool column = (s.flexDirection == "column");
    int gap = s.flexGap;
    if (getenv("MB_FLEXDBG")) {
        std::string cl = node->attrs.count("class")
                             ? node->attrs["class"] : "";
        std::fprintf(stderr, "[flex] enter <%s class='%.50s'> y=%d w=%d"
                             " col=%d gap=%d align=%s justify=%s\n",
                     node->tag.c_str(), cl.c_str(), y, contentW,
                     (int)column, gap, s.alignItems.c_str(),
                     s.justifyContent.c_str());
    }

    // Item = element child (skip display:none children at layout time).
    struct Item { size_t b0, b1, l0, l1; int w, h; };
    std::vector<Item> items;

    int rowTop = y;
    for (auto& c : node->children) {
        if (kNoRender.count(c->tag)) continue;
        // Whitespace-only text between flex items creates no anonymous
        // box in real browsers. Layouting it as an item measured it at
        // the full container width and pushed every following item
        // right — Wikipedia's headers marched off-screen.
        if (c->tag == "text") {
            bool wsOnly = true;
            for (char ch : c->text)
                if (!std::isspace((unsigned char)ch)) { wsOnly = false; break; }
            if (wsOnly) continue;
        }
        Style cs = computeStyle(s, c, cssRules);
        if (cs.hasDisplay && cs.display == "none") continue;
        // Out-of-flow children are not flex items; the container's own
        // absolute pass places them after the flex layout.
        if (cs.position == "absolute" || cs.position == "fixed") continue;

        Item it{};
        it.b0 = boxes.size();
        it.l0 = links.size();

        // Natural width via a throwaway layout (row direction only; column
        // stretches to full width anyway).
        int natural = contentW;
        if (!column) {
            std::vector<Box> scratchBoxes;
            std::vector<Link> scratchLinks;
            std::vector<FloatRegion> scratchFloats;
            int sy = 0;
            int effW = contentW;
            if ((cs.declared & B_WIDTH) && cs.width >= 0)
                effW = cs.width + (int)std::lround(cs.widthPct * contentW / 100.0f);
            layoutNode(c, s, contentX, sy, effW, measureFont, cssRules,
                       scratchBoxes, scratchLinks, linkHref, scratchFloats);
            natural = measureRangeNaturalW(scratchBoxes, 0, scratchBoxes.size());
            natural = std::max(20, std::min(natural, contentW));
        }

        int w = column ? contentW : natural;
        int ty = 0;   // lay items at origin, shift afterwards
        layoutNode(c, s, contentX, ty, w, measureFont, cssRules,
                   boxes, links, linkHref, floats);
        it.b1 = boxes.size();
        it.l1 = links.size();

        int maxR = contentX, minL = contentX + 100000;
        for (size_t i = it.b0; i < it.b1; ++i) {
            minL = std::min(minL, boxes[i].x);
            maxR = std::max(maxR, boxes[i].x + boxes[i].w);
        }
        it.w = std::max(0, maxR - contentX);
        it.h = std::max(1, ty);
        items.push_back(it);
    }

    if (items.empty()) return;

    if (!column) {
        // ---- row axis ----
        int totalW = 0, maxH = 0;
        for (auto& it : items) { totalW += it.w; maxH = std::max(maxH, it.h); }
        totalW += gap * (int)(items.size() - 1);
        int free = std::max(0, contentW - totalW);
        int cursor = contentX;
        int extra = 0, evenGap = gap;
        if (s.justifyContent == "center")          cursor += free / 2;
        else if (s.justifyContent == "flex-end")   cursor += free;
        else if (s.justifyContent == "space-between" && items.size() > 1)
            evenGap = gap + free / (int)(items.size() - 1);
        else if (s.justifyContent == "space-around")  extra = free / (int)(items.size() * 2);
        else if (s.justifyContent == "space-evenly")  extra = free / (int)(items.size() + 1);

        cursor += extra;
        for (size_t k = 0; k < items.size(); ++k) {
            auto& it = items[k];
            int dy = 0;
            if (s.alignItems == "center")       dy = (maxH - it.h) / 2;
            else if (s.alignItems == "flex-end") dy = maxH - it.h;
            if (getenv("MB_FLEXDBG")) {
                int preY = 1 << 30;
                for (size_t i = it.b0; i < it.b1; ++i)
                    if (!boxes[i].lines.empty() || boxes[i].isImage)
                        preY = std::min(preY, boxes[i].y);
                std::fprintf(stderr, "[flex] row item=%zu b=[%zu,%zu) rowTop=%d"
                                     " preY=%d dx=%d dy=%d h=%d\n",
                             k, it.b0, it.b1, rowTop, preY,
                             cursor - contentX, rowTop + dy, it.h);
            }
            shiftRange(boxes, links, it.b0, it.b1, it.l0, it.l1,
                       cursor - contentX, rowTop + dy);
            cursor += it.w + evenGap + (extra ? extra : 0);
        }
        y = rowTop + maxH + s.margin.bottom;
    } else {
        // ---- column axis ----
        int totalH = 0;
        for (auto& it : items) totalH += it.h;
        totalH += gap * (int)(items.size() - 1);
        int declaredH = -1;
        if ((s.declared & B_HEIGHT) && s.height > 0) declaredH = s.height;
        if ((s.declared & B_MIN_HEIGHT) && s.minHeight > 0)
            declaredH = std::max(declaredH, s.minHeight);
        int free = declaredH > 0 ? std::max(0, declaredH - totalH) : 0;
        int cursor = rowTop;
        int evenGap = gap, extra = 0;
        if (free > 0) {
            if (s.justifyContent == "center")        cursor += free / 2;
            else if (s.justifyContent == "flex-end") cursor += free;
            else if (s.justifyContent == "space-between" && items.size() > 1)
                evenGap = gap + free / (int)(items.size() - 1);
            else if (s.justifyContent == "space-around")
                extra = free / (int)(items.size() * 2);
            else if (s.justifyContent == "space-evenly")
                extra = free / (int)(items.size() + 1);
        }
        cursor += extra;
        for (auto& it : items) {
            int dx = 0;
            if (s.alignItems == "center")        dx = std::max(0, (contentW - it.w) / 2);
            else if (s.alignItems == "flex-end") dx = std::max(0, contentW - it.w);
            // Items were laid at origin y=0 — shift to the cross-size
            // adjusted x and the running main-axis y. (The vertical shift
            // was previously computed into targetY and dropped, leaving
            // every flex-column subtree painted at the top of the page.)
            shiftRange(boxes, links, it.b0, it.b1, it.l0, it.l1,
                       dx, cursor);
            cursor += it.h + evenGap + (extra ? extra : 0);
        }
        y = rowTop + std::max(totalH, declaredH > 0 ? declaredH : totalH)
            + s.margin.bottom;
    }
}

// -------------------------------------------------------------------------
// The main recursive layout
// -------------------------------------------------------------------------

static void layoutNode(const std::shared_ptr<Node>& node,
                       const Style& parentStyle,
                       int x, int& y, int width,
                       TTF_Font* measureFont,
                       const std::vector<CSSRule>& cssRules,
                       std::vector<Box>& boxes,
                       std::vector<Link>& links,
                       std::string linkHref,
                       std::vector<FloatRegion>& floats) {
    if (!node) return;
    if (kNoRender.count(node->tag)) return;

    bool isRootish = (node->tag == "text" || node->tag == "root");
    Style s = isRootish ? parentStyle : computeStyle(parentStyle, node, cssRules);

    // MB_BOXTREE=1 — flow timeline: every block entry with its incoming y.
    static int g_boxTreeDepth = 0;
    struct BoxTreePop { ~BoxTreePop() { --g_boxTreeDepth; } } _btpop;
    if (getenv("MB_BOXTREE") && ++g_boxTreeDepth > 0) {
        std::string cls;
        auto ci = node->attrs.find("class");
        if (ci != node->attrs.end()) {
            cls = ci->second;
            if (cls.size() > 60) cls = cls.substr(0, 60);
        }
        std::fprintf(stderr, "[tree] d=%d <%s class='%s'> y_in=%d w=%d"
                             " disp=%s pos=%s\n",
                     g_boxTreeDepth, node->tag.c_str(), cls.c_str(), y, width,
                     (s.declared & B_DISPLAY) ? s.display.c_str() : "-",
                     s.position.c_str());
    }

    if (!isRootish && (s.declared & B_DISPLAY) && s.display == "none") return;
    // visibility: hidden — subtree lays out nothing (inherits via Style).
    if (!isRootish && s.visibilityHidden) return;
    // sr-only / visually-hidden idioms (clip-path/clip zero-area clip):
    // the element paints nothing — skip the subtree entirely.
    if (!isRootish && s.clippedAway) return;

    if (isCodeContainer(node->tag)) {
        for (auto& c : node->children) {
            if (c->tag != "text")
                layoutNode(c, parentStyle, x, y, width, measureFont,
                           cssRules, boxes, links, linkHref, floats);
        }
        return;
    }
    if (node->tag == "br") return;

    if (node->tag == "hr") {
        y += s.margin.top + 8;
        Box b;
        b.x = x; b.y = y; b.w = std::max(40, width); b.h = 1;
        if ((s.declared & B_HEIGHT) && s.height > 0) b.h = s.height;
        b.style.bg = (s.declared & B_BG) ? s.bg : SDL_Color{180, 180, 180, 255};
        b.style.hasBg = true;
        b.sourceNode = node;
        boxes.push_back(b);
        y += b.h + 9 + s.margin.bottom;
        return;
    }

    if (node->tag == "a" && node->attrs.count("href")) {
        linkHref = node->attrs["href"];
    }

    // ---- images ----
    if (node->tag == "img") {
        if (s.cssFloat != "none") {
            // Floated: place at the current y, register region, don't
            // advance the flow cursor.
            int fy = y + s.margin.top;
            Box b;
            b.isImage = true;
            b.sourceNode = node;
            b.style = s;
            std::string path, alt;
            int natW = 0, natH = 0;
            prepareImage(node, s, cssRules, width, path, natW, natH,
                         b.drawW, b.drawH, alt);
            b.imagePath = path;
            b.imageW = natW;
            b.imageH = natH;
            b.altText = alt;
            int fw = std::min(b.drawW, std::max(20, width));
            int fx = (s.cssFloat == "left")
                   ? x
                   : x + std::max(0, width - fw);
            b.x = fx;
            b.y = fy;
            b.w = fw;
            b.h = b.drawH;
            boxes.push_back(b);
            floats.push_back(FloatRegion{s.cssFloat == "left", fx, fw,
                                         fy, fy + b.h});
            return;
        }
        layoutImage(node, s, x, y, width, cssRules, boxes);
        return;
    }

    // ---- form widgets ----
    if (node->tag == "input") {
        layoutInput(node, s, x, y, width, measureFont, cssRules, boxes);
        return;
    }
    if (node->tag == "select") {
        layoutSelect(node, s, x, y, width, cssRules, boxes);
        return;
    }
    if (node->tag == "textarea") {
        layoutTextarea(node, s, x, y, cssRules, boxes);
        return;
    }

    // ---- internal media players (<video> / <audio>) ----
    if (node->tag == "video" || node->tag == "audio") {
        layoutMedia(node, s, x, y, width, cssRules, boxes);
        return;
    }

    if (node->tag == "html" || node->tag == "root") {
        // html/root are ordinary containing blocks: route them through the
        // block container so direct inline content (text, <img>, <a>…)
        // flows together and floats participate in line wrapping.
        layoutBlockContainer(node, s, parentStyle, x, y, width, measureFont,
                             cssRules, boxes, links, linkHref, floats);
        return;
    }

    if (node->tag == "body") {
        // Real UA default: 8px body margin.
        if (!(s.declared & B_MARGIN)) {
            s.margin.set1(8);
            s.hasMargin = true;
        }
        size_t firstIdx = boxes.size();
        layoutBlockContainer(node, s, parentStyle, x, y, width, measureFont,
                             cssRules, boxes, links, linkHref, floats);
        // UA behavior: the body background paints the whole canvas, so
        // stretch its background box up to the very top of the viewport.
        for (size_t i = firstIdx; i < boxes.size(); ++i) {
            if (boxes[i].isImage || !boxes[i].lines.empty()) continue;
            auto sn = boxes[i].sourceNode.lock();
            if (sn != node) continue;
            int bottom = boxes[i].y + boxes[i].h;
            boxes[i].y = 0;
            boxes[i].h = std::max(bottom, 1);
        }
        return;
    }

    // ---- lists ----
    if (node->tag == "ul" || node->tag == "ol") {
        y += s.margin.top;
        int indent = 20;
        int childX = x + indent;
        int childW = std::max(40, width - indent);
        // list-style: none on the list itself suppresses child markers.
        for (auto& c : node->children)
            layoutNode(c, s, childX, y, childW, measureFont, cssRules,
                       boxes, links, linkHref, floats);
        y += s.margin.bottom;
        return;
    }

    if (node->tag == "li") {
        y += s.margin.top;
        // Same float-crush guard as block containers: list items beside
        // floats (sidebar portlets) must not wrap into 2-char columns.
        if (!floats.empty() && s.cssFloat == "none") {
            int liLineH = (s.hasLineHeight && s.lineHeight > 0)
                              ? (int)std::lround(s.lineHeight)
                              : s.fontSize + 6;
            int crushDy = floatCrushDropDy(floats, x, width, y,
                                           std::max(2 * liLineH, 44));
            if (crushDy > 0) y += crushDy;
        }
        auto parent = node->parent.lock();
        bool suppressMarker = (s.listStyleType == "none") ||
                              (parent && parent->tag == "ul" &&
                               parent.get() && false);
        // The parent's style carries list-style when the <ul> declared it.
        if (parent && (parent->tag == "ul" || parent->tag == "ol")) {
            Style ps = computeStyle(Style(), parent, cssRules);
            if (ps.listStyleType == "none") suppressMarker = true;
        }
        if (!suppressMarker) {
            if (parent && parent->tag == "ul") {
                Run bullet;
                bullet.text = "\xe2\x80\xa2  ";
                bullet.style = s;
                // Marker handled below by prefixing runs.
                std::vector<Run> runs;
                collectRuns(node, s, false, false, linkHref,
                            std::weak_ptr<Node>{}, cssRules, width, runs);
                if (!runs.empty()) runs.insert(runs.begin(), bullet);
                WrapResult wr = wrapRuns(runs, x, width, y,
                                         (s.hasLineHeight && s.lineHeight > 0)
                                             ? (int)std::lround(s.lineHeight)
                                             : s.fontSize + 6,
                                         floats, s);
                Box b;
                b.x = x; b.y = y; b.w = width;
                b.sourceNode = node;
                b.style = s;
                fillBoxFromLines(b, wr.lines, s);
                b.h += s.padding.vertical() + s.border.vertical();
                boxes.push_back(b);
                emitLinkRects(b, s, links);
                y += b.h + s.margin.bottom;
                return;
            }
            if (parent && parent->tag == "ol") {
                int idx = 1;
                for (auto& c : parent->children) {
                    if (c.get() == node.get()) break;
                    if (c->tag == "li") ++idx;
                }
                Run marker;
                marker.text = std::to_string(idx) + ". ";
                marker.style = s;
                std::vector<Run> runs;
                collectRuns(node, s, false, false, linkHref,
                            std::weak_ptr<Node>{}, cssRules, width, runs);
                if (!runs.empty()) runs.insert(runs.begin(), marker);
                WrapResult wr = wrapRuns(runs, x, width, y,
                                         (s.hasLineHeight && s.lineHeight > 0)
                                             ? (int)std::lround(s.lineHeight)
                                             : s.fontSize + 6,
                                         floats, s);
                Box b;
                b.x = x; b.y = y; b.w = width;
                b.sourceNode = node;
                b.style = s;
                fillBoxFromLines(b, wr.lines, s);
                b.h += s.padding.vertical() + s.border.vertical();
                boxes.push_back(b);
                emitLinkRects(b, s, links);
                y += b.h + s.margin.bottom;
                return;
            }
        }
        // Suppressed marker or non-list parent: fall through to the
        // generic block layout below by treating it like a div.
        s.display = "block";
    }

    // ---- tables ----
    if (node->tag == "table") {
        layoutTable(node, s, x, y, width, measureFont, cssRules,
                    boxes, links, linkHref, floats);
        return;
    }

    // ---- everything else is a block container ----
    layoutBlockContainer(node, s, parentStyle, x, y, width, measureFont,
                         cssRules, boxes, links, linkHref, floats);
}

// PART4_MARKER
// -------------------------------------------------------------------------
// Block containers (p, div, headings, unknown HTML5 tags, ...)
// -------------------------------------------------------------------------

// Place a block-level floated child (e.g. a sidebar div) at the current y
// and register a float region so sibling text wraps around it.
static void placeBlockFloat(const std::shared_ptr<Node>& child,
                            const Style& cs, const Style& parentStyle,
                            int contentX, int y0, int contentW,
                            TTF_Font* measureFont,
                            const std::vector<CSSRule>& cssRules,
                            std::vector<Box>& boxes,
                            std::vector<Link>& links,
                            std::string linkHref,
                            std::vector<FloatRegion>& floats) {
    size_t b0 = boxes.size(), l0 = links.size();
    std::vector<FloatRegion> innerFloats;
    int sy = y0;
    int w = contentW;
    if ((cs.declared & B_WIDTH) && cs.width >= 0)
        w = cs.width + (int)std::lround(cs.widthPct * contentW / 100.0f);
    layoutNode(child, parentStyle, contentX, sy, w, measureFont, cssRules,
               boxes, links, linkHref, innerFloats);
    int fh = sy - y0;
    int fw = 0;
    {
        int maxR = contentX;
        for (size_t i = b0; i < boxes.size(); ++i)
            maxR = std::max(maxR, boxes[i].x + boxes[i].w);
        fw = maxR - contentX;
    }
    // Shrink-to-fit for text blocks without an explicit width: re-layout
    // at the natural (max line) width so the float doesn't cover the
    // whole column.
    if (!(cs.declared & B_WIDTH)) {
        int nat = measureRangeNaturalW(boxes, b0, boxes.size());
        if (nat > 20 && nat < contentW * 4 / 5) {
            boxes.resize(b0);
            links.resize(l0);
            sy = y0;
            layoutNode(child, parentStyle, contentX, sy, nat, measureFont,
                       cssRules, boxes, links, linkHref, innerFloats);
            fh = sy - y0;
            int maxR = contentX;
            for (size_t i = b0; i < boxes.size(); ++i)
                maxR = std::max(maxR, boxes[i].x + boxes[i].w);
            fw = maxR - contentX;
        }
    }
    int fx = (cs.cssFloat == "left") ? contentX
                                     : contentX + std::max(0, contentW - fw);
    shiftRange(boxes, links, b0, boxes.size(), l0, links.size(),
               fx - contentX, 0);
    floats.push_back(FloatRegion{cs.cssFloat == "left", fx, fw, y0, y0 + fh});
    if (getenv("MB_FLOATSRC"))
        std::fprintf(stderr, "[floatsrc] <%s class='%s' declaredW=%d>"
                             " -> x=%d w=%d y=%d..%d (contentW=%d)\n",
                     child->tag.c_str(),
                     (child->attrs.count("class")
                       ? child->attrs["class"].c_str() : ""),
                     (cs.declared & B_WIDTH) ? 1 : 0,
                     fx, fw, y0, y0 + fh, contentW);
}

static void layoutBlockContainer(const std::shared_ptr<Node>& node,
                                 const Style& s, const Style& parentStyle,
                                 int x, int& y, int width,
                                 TTF_Font* measureFont,
                                 const std::vector<CSSRule>& cssRules,
                                 std::vector<Box>& boxes,
                                 std::vector<Link>& links,
                                 std::string linkHref,
                                 std::vector<FloatRegion>& floats) {
    // ---- effective margins (px + %) ----
    int marT = s.margin.top;
    int marB = s.margin.bottom;
    int marL = s.margin.left +
               (int)std::lround(s.marginPct.left * width / 100.0f);
    int marR = s.margin.right +
               (int)std::lround(s.marginPct.right * width / 100.0f);
    y += marT;

    // ---- effective width ----
    int avail = std::max(40, width - marL - marR);
    int effW = avail;
    if (getenv("MB_NARROWTRACE") && width < 60)
        std::fprintf(stderr, "[narrowin] <%s class='%s'> width=%d y=%d\n",
                     node->tag.c_str(),
                     (node->attrs.count("class")
                       ? node->attrs["class"].c_str() : ""),
                     width, y);
    if ((s.declared & B_WIDTH) && s.width >= 0)
        effW = s.width + (int)std::lround(s.widthPct * width / 100.0f);
    if ((s.declared & B_MAX_WIDTH) && s.maxWidth >= 0)
        effW = std::min(effW, s.maxWidth +
                   (int)std::lround(s.maxWidthPct * width / 100.0f));
    if ((s.declared & B_MIN_WIDTH) && s.minWidth >= 0)
        effW = std::max(effW, s.minWidth +
                   (int)std::lround(s.minWidthPct * width / 100.0f));
    // The 40px floor guards against pathological zero-width containers,
    // but an explicitly declared width (the sr-only "width:1px" idiom)
    // must be honored — floor it only when no width was declared.
    if (s.declared & B_WIDTH) {
        effW = std::min(effW, std::max(1, width));
    } else {
        effW = std::max(40, std::min(effW, std::max(40, width)));
    }
    if (effW > avail && !(s.declared & B_WIDTH)) effW = avail;

    // ---- margin auto centering ----
    int boxX = x + marL;
    if (s.marginAutoLeft && s.marginAutoRight && effW < avail)
        boxX = x + marL + (avail - effW) / 2;
    else if (s.marginAutoRight && !s.marginAutoLeft)
        boxX = x + marL + (avail - effW);

    // ---- padding / border ----
    int padT = s.padding.top;
    int padL = s.padding.left +
               (int)std::lround(s.paddingPct.left * effW / 100.0f);
    int padR = s.padding.right +
               (int)std::lround(s.paddingPct.right * effW / 100.0f);
    int bdT = s.border.top;
    int bdL = s.border.left, bdR = s.border.right;

    // box-sizing: border-box — padding/border eat into the declared width.
    if (s.boxSizing == "border-box" && (s.declared & B_WIDTH) && s.width >= 0) {
        int inner = effW - padL - padR - bdL - bdR;
        if (inner >= 40) {
            // keep effW; content shrinks (contentW below handles it)
        } else {
            effW = 40 + padL + padR + bdL + bdR;
        }
    }

    int contentX = boxX + padL + bdL;
    int contentW = std::max(40, effW - padL - padR - bdL - bdR);

    int childStartY = y;
    size_t firstBoxIdx = boxes.size();
    size_t firstLinkIdx = links.size();

    if (getenv("MB_STYLEDUMP")) {
        std::string cl = node->attrs.count("class")
                             ? node->attrs["class"] : "";
        std::string want = getenv("MB_STYLEDUMP");
        if (cl.find(want) != std::string::npos)
            std::fprintf(stderr,
                "[style] <%s class='%s'> display='%s' w=%d(was %d)"
                " min=%d max=%d tplCols='%s' flex dir=%s pos=%s"
                " float=%s padT=%d\n",
                node->tag.c_str(), cl.c_str(), s.display.c_str(), effW,
                width, s.minWidth, s.maxWidth,
                s.gridTemplateColumns.c_str(),
                s.flexDirection.c_str(), s.position.c_str(),
                s.cssFloat.c_str(), s.padding.top);
    }

    // ---- flex container ----
    if (s.display == "flex" || s.display == "inline-flex") {
        // Flex items are independent block-formatting contexts (CSS
        // Flexbox §4): floats inside them must never leak out to sibling
        // content. Wikipedia's Vector header ships `.search-toggle
        // { float:left }`; without isolation that single rule poisoned
        // every narrow box on the page (crushed Arabic sidebars).
        std::vector<FloatRegion> flexFloats;
        layoutFlexChildren(node, s, contentX, y, contentW, measureFont,
                           cssRules, boxes, links, linkHref, flexFloats);
        int containerH = y - childStartY;
        if ((s.declared & B_MIN_HEIGHT) && s.minHeight > 0)
            containerH = std::max(containerH, s.minHeight);
        if ((s.declared & B_HEIGHT) && s.height > 0)
            containerH = std::max(containerH, s.height);
        y = childStartY + containerH;
        if ((s.bg.r != 255 || s.bg.g != 255 || s.bg.b != 255 ||
             (s.hasBgImage && !s.bgImageUrl.empty())) &&
            !(s.declared & B_BORDER)) {
            // flex children already positioned; paint bg behind them
            Box bg;
            bg.x = boxX; bg.y = childStartY; bg.w = effW; bg.h = containerH;
            bg.style = s;
            bg.bgImagePath = resolveResourceUrl(s.bgImageUrl);
            bg.sourceNode = node;
            do { ScopedClock _c(g_tInsert); boxes.insert(boxes.begin() + firstBoxIdx, bg); } while(0);
        } else if (s.bg.r != 255 || s.bg.g != 255 || s.bg.b != 255 ||
                   (s.hasBgImage && !s.bgImageUrl.empty())) {
            Box bg;
            bg.x = boxX; bg.y = childStartY; bg.w = effW; bg.h = containerH;
            bg.style = s;
            bg.bgImagePath = resolveResourceUrl(s.bgImageUrl);
            bg.sourceNode = node;
            do { ScopedClock _c(g_tInsert); boxes.insert(boxes.begin() + firstBoxIdx, bg); } while(0);
        }
        y += marB;
        return;
    }

    // ---- grid container ----
    if (s.display == "grid" || s.display == "inline-grid") {
        // Same BFC isolation as flex (CSS Grid §4).
        std::vector<FloatRegion> gridFloats;
        layoutGridChildren(node, s, contentX, y, contentW, measureFont,
                           cssRules, boxes, links, linkHref, gridFloats);
        int containerH = y - childStartY;
        if ((s.declared & B_MIN_HEIGHT) && s.minHeight > 0)
            containerH = std::max(containerH, s.minHeight);
        if ((s.declared & B_HEIGHT) && s.height > 0)
            containerH = std::max(containerH, s.height);
        y = childStartY + containerH;
        if ((s.bg.r != 255 || s.bg.g != 255 || s.bg.b != 255 ||
             (s.hasBgImage && !s.bgImageUrl.empty())) &&
            !(s.declared & B_BORDER)) {
            Box bg;
            bg.x = boxX; bg.y = childStartY; bg.w = effW; bg.h = containerH;
            bg.style = s;
            bg.bgImagePath = resolveResourceUrl(s.bgImageUrl);
            bg.sourceNode = node;
            do { ScopedClock _c(g_tInsert); boxes.insert(boxes.begin() + firstBoxIdx, bg); } while(0);
        } else if (s.bg.r != 255 || s.bg.g != 255 || s.bg.b != 255 ||
                   (s.hasBgImage && !s.bgImageUrl.empty())) {
            Box bg;
            bg.x = boxX; bg.y = childStartY; bg.w = effW; bg.h = containerH;
            bg.style = s;
            bg.bgImagePath = resolveResourceUrl(s.bgImageUrl);
            bg.sourceNode = node;
            do { ScopedClock _c(g_tInsert); boxes.insert(boxes.begin() + firstBoxIdx, bg); } while(0);
        }
        y += marB;
        return;
    }

    // One computeStyle per child, shared by the float / block / widget /
    // absolute loops below (they used to recompute the same style up to
    // four times per child).
    std::unordered_map<Node*, Style> childStyleCache;
    auto childStyle = [&](const std::shared_ptr<Node>& c) -> Style& {
        auto it = childStyleCache.find(c.get());
        if (it != childStyleCache.end()) return it->second;
        return childStyleCache.emplace(c.get(), computeStyle(s, c, cssRules))
                   .first->second;
    };

    // ---- float crush avoidance (CSS 2.1 §9.5.2) --------------------------
    // A narrow block whose whole top region is covered by an overlapping
    // float gets its line width crushed to the 20px floor -> one-character
    // -per-line vertical garbage (broken Arabic portlets, badges). CSS
    // never lets line boxes shrink to nothing: they shift DOWN below the
    // float instead. Partial overlaps (text wrapping around an image tail)
    // keep the classic wrap-around. Runs after the flex/grid branches so
    // it only touches regular block-flow containers.
    if (!floats.empty() && s.cssFloat == "none" &&
        s.position != "absolute" && s.position != "fixed") {
        int lineH0 = (s.hasLineHeight && s.lineHeight > 0)
                         ? (int)std::lround(s.lineHeight) : s.fontSize + 6;
        int probeH = std::max(3 * lineH0, 60);
        int crushDy = floatCrushDropDy(floats, contentX, contentW, y, probeH);
        if (crushDy > 0) {
            y += crushDy;
            childStartY += crushDy;
            if (getenv("MB_CRUSHDBG"))
                std::fprintf(stderr,
                    "[crush] <%s class='%s'> dropped %dpx below float\n",
                    node->tag.c_str(),
                    (node->attrs.count("class")
                      ? node->attrs["class"].c_str() : ""), crushDy);
        }
    }

    // ---- floated children (non-image blocks; images are handled inside
    // layoutNode via their own float branch) ----
    for (auto& c : node->children) {
        Style& cs = childStyle(c);
        if ((cs.declared & B_DISPLAY) && cs.display == "none") continue;
        if (cs.visibilityHidden) continue;
        if (cs.cssFloat == "none") continue;
        if (c->tag == "img") {
            // Floated image: layoutNode's img branch places the box and
            // registers the FloatRegion so text wraps around it.
            layoutNode(c, s, contentX, childStartY, contentW, measureFont,
                       cssRules, boxes, links, linkHref, floats);
            continue;
        }
        placeBlockFloat(c, cs, s, contentX, childStartY, contentW,
                        measureFont, cssRules, boxes, links, linkHref, floats);
    }

    // ---- inline runs of this container (and inline descendants) ----
    // Block-in-inline spots (e.g. <span><div style="float:right">…) split
    // the run stream into segments; each segment wraps as its own line
    // group and the block lays out in flow at the spot's position.
    int textH = 0;
    {
        std::vector<Run> runs;
        std::vector<InlineBlockSpot> spots;
        collectRuns(node, s, false, false, linkHref, std::weak_ptr<Node>{},
                    cssRules, contentW, runs, false, &spots);
        if (!runs.empty() || !spots.empty()) {
            int lineH = (s.hasLineHeight && s.lineHeight > 0)
                          ? (int)std::lround(s.lineHeight) : s.fontSize + 6;
            int flowY = childStartY + padT + bdT;
            size_t segStart = 0;
            if (getenv("MB_WRAPDBG"))
                std::fprintf(stderr, "[wrapseg] <%s> runs=%zu spots=%zu"
                                     " contentX=%d contentW=%d\n",
                             node->tag.c_str(), runs.size(), spots.size(),
                             contentX, contentW);
            auto flushSegment = [&](size_t endPos, bool skipMarker) {
                if (endPos > segStart) {
                    std::vector<Run> seg(runs.begin() + segStart,
                                         runs.begin() + endPos);
                    WrapResult wr = wrapRuns(seg, contentX, contentW, flowY,
                                             lineH, floats, s);
                    if (!wr.lines.empty()) {
                        Box b;
                        b.x = boxX; b.y = flowY; b.w = effW;
                        b.sourceNode = node;
                        b.style = s;
                        fillBoxFromLines(b, wr.lines, s);
                        boxes.push_back(b);
                        emitLinkRects(b, s, links);
                        if (b.h > 0) flowY = b.y + b.h;
                    }
                }
                if (skipMarker) segStart = endPos + 1;   // skip "\n" marker
                else            segStart = endPos;
            };
            for (auto& sp : spots) {
                flushSegment(sp.runPos, true);
                // The block itself (float / plain block) in flow order.
                Style spStyle = computeStyle(sp.parentStyle, sp.node,
                                             cssRules);
                if (spStyle.cssFloat != "none") {
                    // Floated block-in-inline (Wikipedia thumbs): same
                    // path as a floated direct child, so it registers its
                    // float region and later text wraps around it.
                    placeBlockFloat(sp.node, spStyle, sp.parentStyle,
                                    contentX, flowY, contentW, measureFont,
                                    cssRules, boxes, links, linkHref,
                                    floats);
                } else {
                    layoutNode(sp.node, sp.parentStyle, contentX, flowY,
                               contentW, measureFont, cssRules, boxes, links,
                               linkHref, floats);
                }
            }
            flushSegment(runs.size(), false);
            // Block children below continue at flowY: express it relative
            // to childStartY (padT/bdT were consumed by flowY's start).
            textH = std::max(0, flowY - childStartY);
        }
    }
    y = childStartY + textH;

    // ---- block-level children ----
    static const std::unordered_set<std::string> kWidgets = {
        "input", "select", "textarea", "button", "video", "audio"
    };
    for (auto& c : node->children) {
        if (kInlineTags.count(c->tag)) continue;   // consumed by the runs
        if (kWidgets.count(c->tag)) continue;      // widgets go last
        Style& cs = childStyle(c);
        if (getenv("MB_BLOCKDBG")) {
            std::string cl = c->attrs.count("class") ? c->attrs["class"] : "";
            std::fprintf(stderr, "[blk] <%s class='%.40s'> disp=%s pos=%s"
                                 " float=%s vis=%d clip=%d\n",
                         c->tag.c_str(), cl.c_str(),
                         (cs.declared & B_DISPLAY) ? cs.display.c_str() : "-",
                         cs.position.c_str(), cs.cssFloat.c_str(),
                         (int)cs.visibilityHidden, (int)cs.clippedAway);
        }
        if ((cs.declared & B_DISPLAY) && cs.display == "none") continue;
        if (cs.cssFloat != "none") continue;  // placed above
        // Out-of-flow: absolute/fixed children are placed by the
        // dedicated pass below. Laying them here as well pushed the
        // flow cursor by their full height AND painted them twice
        // (GitHub's fixed header wrapper consumed ~430px of flow).
        if (cs.position == "absolute" || cs.position == "fixed") continue;
        if ((cs.declared & B_DISPLAY) &&
            (cs.display == "inline")) continue;
        layoutNode(c, s, contentX, y, contentW, measureFont, cssRules,
                   boxes, links, linkHref, floats);
    }

    // ---- replaced widgets (kept visible after the text, as before) ----
    for (auto& c : node->children) {
        if (!kWidgets.count(c->tag)) continue;
        Style& cs = childStyle(c);
        if ((cs.declared & B_DISPLAY) && cs.display == "none") continue;
        if (cs.position == "absolute" || cs.position == "fixed") continue;
        layoutNode(c, s, contentX, y, contentW, measureFont, cssRules,
                   boxes, links, linkHref, floats);
    }

    // ---- absolutely positioned children (laid last, painted on top) ----
    for (auto& c : node->children) {
        Style& cs = childStyle(c);
        if (cs.position != "absolute" && cs.position != "fixed") continue;
        size_t b0 = boxes.size(), l0 = links.size();
        int sy = childStartY;
        std::vector<FloatRegion> absFloats;
        layoutNode(c, cs, contentX, sy, contentW, measureFont, cssRules,
                   boxes, links, linkHref, absFloats);
        int minL = 1 << 20, maxR = -(1 << 20);
        int minT = 1 << 20;
        for (size_t i = b0; i < boxes.size(); ++i) {
            minL = std::min(minL, boxes[i].x);
            maxR = std::max(maxR, boxes[i].x + boxes[i].w);
            minT = std::min(minT, boxes[i].y);
        }
        if (b0 == boxes.size()) { minL = contentX; maxR = contentX; minT = sy; }
        int w = std::max(0, maxR - minL);
        int h = std::max(0, sy - minT);
        // Explicit width/height on an out-of-flow box wins over the
        // measured extent — the sr-only "1x1 + overflow:hidden" idiom
        // only hides when the box really is 1x1.
        if ((cs.declared & B_WIDTH) && cs.width >= 0)
            w = cs.width + (int)std::lround(cs.widthPct * contentW / 100.0f);
        if ((cs.declared & B_HEIGHT) && cs.height >= 0)
            h = cs.height;
        int dx = 0, dy = 0;
        if (cs.hasOffLeft)
            dx = contentX + cs.offLeft +
                 (int)std::lround(cs.offLeftPct * contentW / 100.0f) - minL;
        else if (cs.hasOffRight)
            dx = boxX + effW - cs.offRight - w - minL;
        if (cs.hasOffTop)
            dy = childStartY + cs.offTop - minT;
        else if (cs.hasOffBottom)
            dy = childStartY - cs.offBottom - h;   // relative to container top
        if (dx || dy) shiftRange(boxes, links, b0, boxes.size(),
                                 l0, links.size(), dx, dy);
    }

    int containerH = std::max(0, y - childStartY);
    if ((s.declared & B_MIN_HEIGHT) && s.minHeight > 0)
        containerH = std::max(containerH, s.minHeight);
    if ((s.declared & B_HEIGHT) && s.height >= 0)
        containerH = std::max(containerH, s.height);   // incl. height:0
    if ((s.declared & B_MAX_HEIGHT) && s.maxHeight > 0)
        containerH = std::min(containerH, s.maxHeight);
    y = childStartY + containerH;

    // ---- background box behind the children ----
    if (s.bg.r != 255 || s.bg.g != 255 || s.bg.b != 255 ||
        (s.hasBgImage && !s.bgImageUrl.empty())) {
        Box bg;
        bg.x = boxX; bg.y = childStartY; bg.w = effW; bg.h = containerH;
        bg.style = s;
        bg.bgImagePath = resolveResourceUrl(s.bgImageUrl);
        bg.sourceNode = node;
        do { ScopedClock _c(g_tInsert); boxes.insert(boxes.begin() + firstBoxIdx, bg); } while(0);
    }

    // ---- position: relative shifts the whole container ----
    if (s.position == "relative" &&
        (s.hasOffTop || s.hasOffLeft ||
         s.offTopPct != 0 || s.offLeftPct != 0)) {
        int dx = s.offLeft + (int)std::lround(s.offLeftPct * width / 100.0f);
        int dy = s.offTop;
        if (dx || dy)
            shiftRange(boxes, links, firstBoxIdx, boxes.size(),
                       firstLinkIdx, links.size(), dx, dy);
    }

    y += marB;
}

// ---------------------------------------------------------------------------
// Text selection support (round 3)
//
// Geometry mirrors the renderer exactly (same line-origin math, same
// font resolution, same measurement) so the caret the user drags lands
// where the text actually paints. Byte offsets index the concatenated
// run text of a line; image runs occupy width but contribute no bytes.
// ---------------------------------------------------------------------------

static int selectionLineHeight(const Box& b, int li) {
    return (li >= 0 && li < (int)b.lineHeights.size())
               ? b.lineHeights[li] : b.style.fontSize + 6;
}

// Y of the line's top in document coords (same origin as the renderer).
static int selectionLineTop(const Box& b, int li) {
    int y = b.y + 2 + b.style.padding.top + b.style.border.top;
    for (int i = 0; i < li; ++i) y += selectionLineHeight(b, i);
    return y;
}

int lineByteLength(const Box& b, int lineIdx) {
    if (lineIdx < 0 || lineIdx >= (int)b.lines.size()) return 0;
    int n = 0;
    for (auto& r : b.lines[lineIdx])
        if (!r.isImage) n += (int)r.text.size();
    return n;
}

// Concatenated run text of a line (visual run order — same order the
// byte offsets index).
static std::string selectionLineText(const Box& b, int lineIdx) {
    std::string s;
    if (lineIdx < 0 || lineIdx >= (int)b.lines.size()) return s;
    for (auto& r : b.lines[lineIdx])
        if (!r.isImage) s += r.text;
    return s;
}

// Clamp a byte offset onto a UTF-8 codepoint boundary (never split a
// multi-byte sequence).
static int snapToCodepoint(const std::string& s, int byte) {
    if (byte <= 0) return 0;
    if (byte >= (int)s.size()) return (int)s.size();
    int i = byte;
    while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) --i;
    return i;
}

TextPos hitTestText(const LayoutResult& lr, TTF_Font* defaultFont,
                    int x, int y) {
    TextPos best;
    int bestDist = 1 << 30;

    for (size_t bi = 0; bi < lr.boxes.size(); ++bi) {
        const Box& b = lr.boxes[bi];
        if (b.isImage || b.lines.empty()) continue;
        if (b.style.visibilityHidden || b.style.opacity <= 0.001f) continue;

        // Effective text alignment — resolved exactly like the renderer
        // resolves it, so hit positions land on the painted glyphs even
        // for centered/right-aligned/RTL lines.
        std::string ta = b.style.textAlign;
        if (!b.style.hasTextAlign || ta == "start")
            ta = b.rtl ? "right" : "left";
        else if (ta == "end")
            ta = b.rtl ? "left" : "right";

        for (int li = 0; li < (int)b.lines.size(); ++li) {
            int lineTop = selectionLineTop(b, li);
            int lineH = selectionLineHeight(b, li);

            // Vertical match: inside the line, or the nearest line when
            // the click lands between/after lines. Distance 0 = the
            // point is inside the line box.
            int dy = 0;
            if (y < lineTop)               dy = lineTop - y;
            else if (y >= lineTop + lineH) dy = y - (lineTop + lineH - 1);

            int lineLeftInset = 0, lineRightInset = 0;
            if (li < (int)b.lineInsets.size()) {
                lineLeftInset  = std::max(0, b.lineInsets[li].first);
                lineRightInset = std::max(0, b.lineInsets[li].second);
            }

            // Pass 1: total line width (renderer's measure loop).
            int lineWidth = 0;
            for (auto& r : b.lines[li]) {
                if (r.isImage) { lineWidth += r.drawW; continue; }
                if (r.text.empty()) continue;
                TTF_Font* rf = getFontForRun(r.text, r.style.fontFamily,
                                             r.style.fontSize, r.style.bold,
                                             r.style.italic, defaultFont);
                int runW = 0, runH = 0;
                if (rf) measureTextCached(rf, r.text, runW, runH);
                lineWidth += runW;
            }
            int contentBoxW = std::max(0, b.w - b.style.padding.horizontal()
                                             - b.style.border.horizontal()
                                             - 8 - lineLeftInset
                                             - lineRightInset);

            // Pass 2: walk runs from the aligned origin.
            int rx = b.x + 4 + b.style.padding.left + b.style.border.left
                     + lineLeftInset;
            if (ta == "center")
                rx += std::max(0, (contentBoxW - lineWidth) / 2);
            else if (ta == "right")
                rx += std::max(0, contentBoxW - lineWidth) + lineRightInset;

            int runStartByte = 0;
            int bestByte = 0;
            int bestByteDist = 1 << 30;

            for (auto& r : b.lines[li]) {
                if (r.isImage) {
                    // An image is an atomic "character": snap to its
                    // edges (it contributes no bytes).
                    int imgRight = rx + r.drawW;
                    int d = (x <= imgRight) ? imgRight - x : x - imgRight;
                    if (d < bestByteDist) {
                        bestByteDist = d;
                        bestByte = runStartByte;
                    }
                    rx = imgRight;
                    continue;
                }
                if (r.text.empty()) continue;
                TTF_Font* rf = getFontForRun(r.text, r.style.fontFamily,
                                             r.style.fontSize, r.style.bold,
                                             r.style.italic, defaultFont);
                int runW = 0, runH = 0;
                if (rf) measureTextCached(rf, r.text, runW, runH);

                if (x <= rx || runW == 0) {
                    int d = (x <= rx) ? rx - x : 0;
                    if (d < bestByteDist) {
                        bestByteDist = d;
                        bestByte = runStartByte;
                    }
                } else if (x >= rx + runW) {
                    int d = x - (rx + runW);
                    if (d < bestByteDist) {
                        bestByteDist = d;
                        bestByte = runStartByte + (int)r.text.size();
                    }
                } else {
                    // Inside the run: nearest codepoint edge by prefix
                    // width. Runs are short, so per-prefix measuring is
                    // fine for an interactive click.
                    int prevW = 0;
                    size_t i = 0;
                    while (i < r.text.size()) {
                        size_t n = 1;
                        while (i + n < r.text.size() &&
                               ((unsigned char)r.text[i + n] & 0xC0) == 0x80)
                            ++n;
                        std::string piece = r.text.substr(0, i + n);
                        int pw = 0, ph = 0;
                        if (rf) measureTextCached(rf, piece, pw, ph);
                        int edgeL = rx + prevW;
                        int edgeR = rx + pw;
                        int dL = (x <= edgeL) ? edgeL - x : x - edgeL;
                        if (dL < bestByteDist) {
                            bestByteDist = dL;
                            bestByte = runStartByte + (int)i;
                        }
                        if (i + n >= r.text.size()) {
                            int dR = (x >= edgeR) ? x - edgeR : edgeR - x;
                            if (dR < bestByteDist) {
                                bestByteDist = dR;
                                bestByte = runStartByte + (int)(i + n);
                            }
                        }
                        prevW = pw;
                        i += n;
                    }
                }
                rx += runW;
                runStartByte += (int)r.text.size();
            }
            std::string whole = selectionLineText(b, li);
            bestByte = snapToCodepoint(whole, bestByte);

            if (dy < bestDist || (dy == bestDist && !best.valid)) {
                bestDist = dy;
                best.valid = true;
                best.box = (int)bi;
                best.line = li;
                best.byte = bestByte;
            }
        }
    }
    return best;
}

std::string textBetween(const LayoutResult& lr, const TextPos& a,
                        const TextPos& b) {
    if (!a.valid || !b.valid) return "";
    TextPos s = a, e = b;
    if (e < s) std::swap(s, e);

    std::string out;
    for (size_t bi = (size_t)std::max(0, s.box);
         bi < lr.boxes.size() && bi <= (size_t)e.box; ++bi) {
        const Box& bx = lr.boxes[bi];
        if (bx.isImage) continue;
        if (bx.lines.empty()) continue;
        int liLo = (bi == (size_t)s.box) ? s.line : 0;
        int liHi = (bi == (size_t)e.box) ? e.line : (int)bx.lines.size() - 1;
        for (int li = liLo; li <= liHi && li < (int)bx.lines.size(); ++li) {
            bool isLast = (bi == (size_t)e.box && li == e.line);
            int lo = (bi == (size_t)s.box && li == s.line) ? s.byte : 0;
            int hi = isLast ? e.byte : lineByteLength(bx, li);
            if (hi > lo) {
                int pos = 0;
                for (auto& r : bx.lines[li]) {
                    if (r.isImage) continue;
                    int rEnd = pos + (int)r.text.size();
                    if (rEnd > lo && pos < hi) {
                        int from = std::max(lo, pos) - pos;
                        int to = std::min(hi, rEnd) - pos;
                        // Prefer the bidi-original text for a FULL run;
                        // byte-sliced runs use the visual text (byte
                        // offsets index it).
                        if (from == 0 && to == (int)r.text.size() &&
                            !r.logical.empty()) {
                            out += r.logical;
                        } else {
                            out += r.text.substr((size_t)from,
                                                 (size_t)(to - from));
                        }
                    }
                    pos = rEnd;
                }
            }
            if (!isLast) out += '\n';   // line break between picked lines
        }
    }
    return out;
}

// -------------------------------------------------------------------------
// Element geometry (round 4) — getBoundingClientRect support
// -------------------------------------------------------------------------

// True when `maybeAncestor` is a strict ancestor of `node` in the DOM.
static bool isStrictAncestor(const Node* maybeAncestor,
                             const std::shared_ptr<Node>& node) {
    auto p = node->parent.lock();
    while (p) {
        if (p.get() == maybeAncestor) return true;
        p = p->parent.lock();
    }
    return false;
}

bool rectForNode(const LayoutResult& lr, TTF_Font* defaultFont,
                 const std::shared_ptr<Node>& node, DOMRect& out) {
    if (!node) return false;
    Node* target = node.get();

    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool any = false;
    auto acc = [&](float l, float t, float r, float b) {
        if (!any) { x0 = l; y0 = t; x1 = r; y1 = b; any = true; return; }
        x0 = std::min(x0, l); y0 = std::min(y0, t);
        x1 = std::max(x1, r); y1 = std::max(y1, b);
    };

    // --- 1. The node's own boxes (blocks, atomic images, form widgets).
    //     This is the exact border-box for everything that owns one.
    for (const auto& b : lr.boxes) {
        auto sn = b.sourceNode.lock();
        if (!sn || sn.get() != target) continue;
        acc((float)b.x, (float)b.y,
            (float)(b.x + b.w), (float)(b.y + b.h));
    }

    // --- 2. Per-line <a> hit-rect fragments (link rects).
    for (const auto& lk : lr.links) {
        auto sn = lk.sourceNode.lock();
        if (!sn || sn.get() != target) continue;
        acc((float)lk.x, (float)lk.y,
            (float)(lk.x + lk.w), (float)(lk.y + lk.h));
    }

    // --- 3. Inline run fragments. Only needed when nothing matched
    //     above: any run lives inside its containing block's box, so a
    //     box-level match already covers it. For inline elements
    //     (<b>/<i>/<span>/...) this is the ONLY source of geometry —
    //     exactly like real browsers, which union the element's line
    //     fragments. Runs produced by descendant inline elements match
    //     too (union-of-fragments semantics for <b><i>x</i></b>).
    if (!any) {
        for (const auto& b : lr.boxes) {
            if (b.isImage || b.lines.empty()) continue;
            if (b.style.visibilityHidden || b.style.opacity <= 0.001f)
                continue;

            // Effective text alignment — resolved exactly like the
            // renderer resolves it (mirrors hitTestText).
            std::string ta = b.style.textAlign;
            if (!b.style.hasTextAlign || ta == "start")
                ta = b.rtl ? "right" : "left";
            else if (ta == "end")
                ta = b.rtl ? "left" : "right";

            for (int li = 0; li < (int)b.lines.size(); ++li) {
                int lineTop = selectionLineTop(b, li);
                int lineH   = selectionLineHeight(b, li);

                int lineLeftInset = 0, lineRightInset = 0;
                if (li < (int)b.lineInsets.size()) {
                    lineLeftInset  = std::max(0, b.lineInsets[li].first);
                    lineRightInset = std::max(0, b.lineInsets[li].second);
                }

                // Pass 1: total line width (renderer's measure loop).
                int lineWidth = 0;
                for (auto& r : b.lines[li]) {
                    if (r.isImage) { lineWidth += r.drawW; continue; }
                    if (r.text.empty()) continue;
                    TTF_Font* rf = getFontForRun(r.text, r.style.fontFamily,
                                                 r.style.fontSize,
                                                 r.style.bold, r.style.italic,
                                                 defaultFont);
                    int runW = 0, runH = 0;
                    if (rf) measureTextCached(rf, r.text, runW, runH);
                    lineWidth += runW;
                }
                int contentBoxW = std::max(0,
                    b.w - b.style.padding.horizontal()
                        - b.style.border.horizontal()
                        - 8 - lineLeftInset - lineRightInset);

                // Pass 2: walk runs from the aligned origin, keeping the
                // rect of every run this node (or a descendant inline of
                // this node) produced.
                int rx = b.x + 4 + b.style.padding.left
                            + b.style.border.left + lineLeftInset;
                if (ta == "center")
                    rx += std::max(0, (contentBoxW - lineWidth) / 2);
                else if (ta == "right")
                    rx += std::max(0, contentBoxW - lineWidth)
                                + lineRightInset;

                for (auto& r : b.lines[li]) {
                    auto owner = r.sourceNode.lock();
                    bool match = owner && (owner.get() == target ||
                                           isStrictAncestor(target, owner));
                    if (match) {
                        if (r.isImage) {
                            // Renderer draws inline images flush with
                            // the bottom of the line box.
                            int imgTop = lineTop +
                                         std::max(0, lineH - r.drawH);
                            acc((float)rx, (float)imgTop,
                                (float)(rx + r.drawW),
                                (float)(imgTop + r.drawH));
                        } else if (!r.text.empty()) {
                            TTF_Font* rf =
                                getFontForRun(r.text, r.style.fontFamily,
                                              r.style.fontSize, r.style.bold,
                                              r.style.italic, defaultFont);
                            int runW = 0, runH = 0;
                            if (rf)
                                measureTextCached(rf, r.text, runW, runH);
                            acc((float)rx, (float)lineTop,
                                (float)(rx + runW),
                                (float)(lineTop + lineH));
                        }
                    }
                    // Advance x exactly like the renderer does.
                    if (r.isImage) {
                        rx += r.drawW;
                    } else if (!r.text.empty()) {
                        TTF_Font* rf = getFontForRun(r.text,
                                                     r.style.fontFamily,
                                                     r.style.fontSize,
                                                     r.style.bold,
                                                     r.style.italic,
                                                     defaultFont);
                        int runW = 0, runH = 0;
                        if (rf) measureTextCached(rf, r.text, runW, runH);
                        rx += runW;
                    }
                }
            }
        }
    }

    if (!any) return false;
    out.x = x0;
    out.y = y0;
    out.width  = x1 - x0;
    out.height = y1 - y0;
    return true;
}

// -------------------------------------------------------------------------
// Entry point
// -------------------------------------------------------------------------

LayoutResult layout(const std::shared_ptr<Node>& root, int width,
                    TTF_Font* font, const std::vector<CSSRule>& cssRules,
                    const std::string& baseDir) {
    // Tell the singleton where to look for relative URLs.
    ResourceLoader::instance().setBaseDir(baseDir);
    // Viewport width drives @media evaluation.
    setMediaViewportWidth(width);

    g_tCompute = g_tInsert = g_tWrap = 0;
    g_nCompute = g_nBoxes = 0;
    g_nMemoHit = 0;
    g_styleMemo.clear();   // styles depend on zoom/:hover — recomputed per pass
    clearSelectorAncCache();
    auto _t0 = std::chrono::steady_clock::now();

    LayoutResult r;
    Style base;
    int y = 0;
    std::vector<FloatRegion> floats;
    if (getenv("MB_WRAPDBG")) {
        std::fprintf(stderr, "[tree] root kids:");
        for (auto& c : root->children)
            std::fprintf(stderr, " <%s>%zu", c->tag.c_str(), c->children.size());
        std::fprintf(stderr, "\n");
    }
    layoutNode(root, base, 0, y, width, font, cssRules,
               r.boxes, r.links, "", floats);
    r.contentHeight = y;

    // MB_TEXTDUMP=1 — print every text box (first line snippet) for
    // locating duplicate/overlapping text layout. MB_NARROWDUMP=1 —
    // print only boxes narrower than 40px (collapsed containers).
    if (getenv("MB_NARROWDUMP")) {
        for (const auto& b : r.boxes) {
            if (b.w >= 40 || b.lines.empty()) continue;
            std::string snippet;
            for (auto& ln : b.lines)
                for (auto& rn : ln) {
                    if (!rn.text.empty() && rn.text != "\n")
                        snippet += rn.text;
                    if (snippet.size() > 40) break;
                }
            if (snippet.empty()) continue;
            auto sn = b.sourceNode.lock();
            std::string cls;
            if (sn) {
                auto ci = sn->attrs.find("class");
                if (ci != sn->attrs.end()) cls = ci->second;
                if (cls.size() > 50) cls = cls.substr(0, 50);
            }
            std::fprintf(stderr, "[narrow] <%s class='%s'> x=%d y=%d w=%d"
                                 " h=%d lines=%zu '%.40s'\n",
                         sn ? sn->tag.c_str() : "?", cls.c_str(),
                         b.x, b.y, b.w, b.h, b.lines.size(),
                         snippet.c_str());
        }
    }
    if (getenv("MB_TEXTDUMP")) {
        for (const auto& b : r.boxes) {
            std::string snippet;
            for (auto& ln : b.lines)
                for (auto& rn : ln) {
                    if (!rn.text.empty() && rn.text != "\n")
                        snippet += rn.text;
                    if (snippet.size() > 60) break;
                }
            if (snippet.empty()) continue;
            auto sn = b.sourceNode.lock();
            std::fprintf(stderr, "[box] <%s> x=%d y=%d w=%d h=%d lines=%zu"
                                 " ovf=%d clip=%d '%.60s'\n",
                         sn ? sn->tag.c_str() : "?",
                         b.x, b.y, b.w, b.h, b.lines.size(),
                         (int)b.style.overflowHidden,
                         (int)b.style.clippedAway, snippet.c_str());
        }
    }

    // MB_FLOATDBG=1 — report text boxes that intersect a float region:
    // every printed line is a real "text painted over a floated image"
    // bug (floats must shrink text lines, never be overlapped by them).
    if (getenv("MB_FLOATDBG")) {
        int reported = 0;
        for (size_t fi = 0; fi < floats.size(); ++fi) {
            const FloatRegion& f = floats[fi];
            if (f.w <= 0 || f.yBottom <= f.yTop) continue;
            for (const auto& b : r.boxes) {
                if (b.isImage) continue;
                bool overlaps = b.x < f.x + f.w && b.x + b.w > f.x &&
                                b.y < f.yBottom && b.y + b.h > f.yTop;
                if (!overlaps) continue;
                // A pure background box (no lines) is fine.
                bool hasText = false;
                for (auto& ln : b.lines)
                    for (auto& rn : ln)
                        if (!rn.isImage && !rn.text.empty()) { hasText = true; break; }
                if (!hasText) continue;
                const char* tag = "?";
                std::string cls;
                if (auto sn = b.sourceNode.lock()) {
                    tag = sn->tag.c_str();
                    auto ci = sn->attrs.find("class");
                    if (ci != sn->attrs.end()) cls = ci->second;
                }
                std::fprintf(stderr,
                    "[floatovl] float#%zu(%s x=%d w=%d y=%d..%d) overlapped"
                    " by <%s class='%s'> box x=%d y=%d w=%d h=%d\n",
                    fi, f.isLeft ? "L" : "R", f.x, f.w, f.yTop, f.yBottom,
                    tag, cls.c_str(), b.x, b.y, b.w, b.h);
                if (++reported > 40) { fi = floats.size(); break; }
            }
        }
    }
    double tot = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - _t0).count();
    std::cerr << "[perf] layout total=" << tot << "ms computeStyle="
              << g_tCompute << "ms(" << g_nCompute << " calls, "
              << g_nMemoHit << " memo hits) wrap="
              << g_tWrap << "ms insert=" << g_tInsert << "ms boxes="
              << r.boxes.size() << "\n";
    return r;
}

} // namespace browser


