#pragma once
#include "../html/parser.h"
#include <SDL2/SDL.h>
#include <string>
#include <vector>
#include <map>

namespace browser {

// Box model edges: 0=top, 1=right, 2=bottom, 3=left
struct Edges {
    int top = 0, right = 0, bottom = 0, left = 0;
    void set1(int v) { top = right = bottom = left = v; }
    void set2(int v, int h) { top = bottom = v; left = right = h; }
    void set3(int t, int h, int b) { top = t; left = right = h; bottom = b; }
    void set4(int t, int r, int b, int l) { top = t; right = r; bottom = b; left = l; }
    int horizontal() const { return left + right; }
    int vertical()   const { return top + bottom; }
};

// Percentage parts of margin/padding (per side, 0..100). Real browsers
// resolve % margins/paddings against the containing block WIDTH (also for
// top/bottom). The px parts live in Style::margin / Style::padding; the
// layout resolves pct against the available width and adds them.
struct PctEdges {
    float top = 0, right = 0, bottom = 0, left = 0;
    bool any() const { return top != 0 || right != 0 || bottom != 0 || left != 0; }
};

// Property-declaration bits. Set by applyStyle when a declaration actually
// parses; used by the layout's computeStyle to merge only the properties a
// matched rule really declares (cascade), replacing the old error-prone
// field-by-field flag copying.
enum StyleBits : uint64_t {
    B_COLOR           = 1ull << 0,
    B_BG              = 1ull << 1,
    B_FONT_SIZE       = 1ull << 2,
    B_BOLD            = 1ull << 3,
    B_ITALIC          = 1ull << 4,
    B_UNDERLINE       = 1ull << 5,
    B_FONT_FAMILY     = 1ull << 6,
    B_LINE_HEIGHT     = 1ull << 7,
    B_TEXT_TRANSFORM  = 1ull << 8,
    B_PRE             = 1ull << 9,
    B_BG_IMAGE        = 1ull << 10,
    B_BG_SIZE         = 1ull << 11,
    B_BG_REPEAT       = 1ull << 12,
    B_BG_POS          = 1ull << 13,
    B_MARGIN          = 1ull << 14,
    B_PADDING         = 1ull << 15,
    B_BORDER          = 1ull << 16,
    B_BORDER_COLOR    = 1ull << 17,
    B_TEXT_ALIGN      = 1ull << 18,
    B_DISPLAY         = 1ull << 19,
    B_WIDTH           = 1ull << 20,
    B_HEIGHT          = 1ull << 21,
    B_MAX_WIDTH       = 1ull << 22,
    B_MIN_WIDTH       = 1ull << 23,
    B_MAX_HEIGHT      = 1ull << 24,
    B_MIN_HEIGHT      = 1ull << 25,
    B_POSITION        = 1ull << 26,
    B_OFFSETS         = 1ull << 27,
    B_FLOAT           = 1ull << 28,
    B_FLEX_DIR        = 1ull << 29,
    B_JUSTIFY         = 1ull << 30,
    B_ALIGN_ITEMS     = 1ull << 31,
    B_GAP             = 1ull << 32,
    B_BOX_SIZING      = 1ull << 33,
    B_OBJECT_FIT      = 1ull << 34,
    B_OPACITY         = 1ull << 35,
    B_LIST_STYLE      = 1ull << 36,
    B_VERTICAL_ALIGN  = 1ull << 37,
    B_VISIBILITY      = 1ull << 38,
    B_DIRECTION       = 1ull << 39,
    B_GRID_TPL        = 1ull << 40,   // grid-template-columns/rows (raw)
    B_GRID_AREAS      = 1ull << 41,   // grid-template-areas (raw)
    B_GRID_AREA       = 1ull << 42,   // grid-area on a child (raw)
    B_GRID_POS        = 1ull << 43,   // grid-column / grid-row on a child
    B_OVERFLOW        = 1ull << 44,   // overflow: hidden/clip
    B_CLIP            = 1ull << 45,   // clip-path/clip zero-area (sr-only)
};

struct Style {
    SDL_Color color = {0, 0, 0, 255};
    SDL_Color bg    = {255, 255, 255, 255};
    int fontSize    = 16;
    bool bold       = false;
    bool italic     = false;
    bool underline  = false;
    std::string fontFamily = "sans-serif";   // "serif", "monospace", or font name
    bool hasFontFamily = false;

    // line-height: 0 = auto (fontSize + 6). > 0 = exact pixel height.
    float lineHeight = 0;
    bool hasLineHeight = false;

    // text-transform: "", "uppercase", "lowercase", "capitalize"
    std::string textTransform;
    bool hasTextTransform = false;

    // white-space: pre — preserve spaces/newlines (also set for <pre>)
    bool pre = false;
    bool hasPre = false;

    // background-image url(...) — raw (unresolved) URL. Empty = none.
    std::string bgImageUrl;
    bool hasBgImage = false;

    Edges margin;
    Edges padding;
    Edges border;          // border width per side
    SDL_Color borderColor  = {0, 0, 0, 255};
    SDL_Color borderTopColor    = {0, 0, 0, 255};
    SDL_Color borderRightColor  = {0, 0, 0, 255};
    SDL_Color borderBottomColor = {0, 0, 0, 255};
    SDL_Color borderLeftColor   = {0, 0, 0, 255};

    // text-align: "left" (default), "center", "right"
    std::string textAlign = "left";
    bool hasTextAlign = false;

    // display: "inline" (default), "block", "none"
    std::string display = "inline";
    bool hasDisplay = false;

    // width / height: -1 = auto. We only support px for now.
    int width = -1;
    int height = -1;
    int maxWidth = -1;
    int minWidth = -1;
    bool hasWidth = false;
    bool hasHeight = false;
    bool hasMaxWidth = false;
    bool hasMinWidth = false;

    // ---- New CSS support -----------------------------------------------
    // Percent variants: width: 50% etc. Resolved against the containing
    // block width at layout time (px parts above stay as-is and the two
    // add up, which also covers calc(100% - 40px): pct=100, px=-40).
    float widthPct = 0, heightPct = 0, maxWidthPct = 0, minWidthPct = 0;
    bool hasWidthPct = false, hasHeightPct = false;
    bool hasMaxWidthPct = false, hasMinWidthPct = false;
    int maxHeight = -1, minHeight = -1;
    bool hasMaxHeight = false, hasMinHeight = false;

    // % parts of margin/padding (resolved against containing width).
    PctEdges marginPct, paddingPct;

    // margin-left/right: auto → centering when a width is set.
    bool marginAutoLeft = false, marginAutoRight = false;

    // position: "static" (default) | "relative" | "absolute" | "fixed".
    // Offsets as px + % pairs (top/left/right/bottom). fixed is drawn
    // like absolute (we don't track viewport-relative drawing).
    std::string position = "static";
    bool hasPosition = false;
    int offTop = 0, offRight = 0, offBottom = 0, offLeft = 0;
    float offTopPct = 0, offRightPct = 0, offBottomPct = 0, offLeftPct = 0;
    bool hasOffTop = false, hasOffRight = false, hasOffBottom = false, hasOffLeft = false;

    // float: "none" | "left" | "right"
    std::string cssFloat = "none";
    bool hasFloat = false;

    // flex container properties (display: flex)
    std::string flexDirection = "row";       // row | column
    std::string justifyContent = "flex-start"; // flex-start|center|space-between|space-around|flex-end
    std::string alignItems = "stretch";      // stretch|center|flex-start|flex-end
    int flexGap = 0;
    bool hasFlexGap = false;

    // box-sizing: "content-box" (default) | "border-box"
    std::string boxSizing = "content-box";
    bool hasBoxSizing = false;

    // object-fit for <img>: "fill" (default stretch) | "contain" | "cover"
    std::string objectFit = "fill";
    bool hasObjectFit = false;

    // background-size / repeat / position for CSS background images.
    std::string bgSize = "auto";             // auto|cover|contain|"<w> <h>" px
    int bgSizeW = 0, bgSizeH = 0;            // px parts when bgSize is explicit
    std::string bgRepeat = "repeat";         // repeat|no-repeat|repeat-x|repeat-y
    std::string bgPosition = "0% 0%";        // kept as parsed percents
    float bgPosX = 0, bgPosY = 0;            // 0..100

    // element opacity 0..1 (applied to background fills and text color)
    float opacity = 1.0f;
    bool hasOpacity = false;

    // list-style-type: none suppresses the bullet/number markers.
    std::string listStyleType = "";
    bool hasListStyle = false;

    // vertical-align for inline contexts: "baseline"(0)|"top"|"middle"|"bottom"
    std::string verticalAlign = "baseline";
    bool hasVerticalAlign = false;

    // visibility: hidden — the subtree paints and lays out nothing.
    // Inherited by default (Style copies it), rules may re-show children
    // with visibility: visible (mergeDeclared overrides).
    bool visibilityHidden = false;
    bool hasVisibility = false;

    // overflow: hidden | clip — content this box paints is clipped to the
    // box rect (renderer-side clip). auto/scroll degrade to hidden.
    bool overflowHidden = false;
    // clip-path: inset(>=50%) / rect(0 0 0 0) or clip: rect(...) with a
    // zero-area rect — the "sr-only / visually-hidden" idiom. The subtree
    // paints and lays out nothing, exactly like visibility:hidden.
    bool clippedAway = false;

    // direction: "ltr" | "rtl" | "auto". Inherited. Set by the CSS
    // `direction` property or the HTML dir attribute (CSS wins when both
    // declare it). "auto" resolves per box via first-strong detection.
    std::string direction = "ltr";
    bool hasDirection = false;

    // ---- CSS grid (basic but real) -------------------------------------
    // Raw template strings, parsed at layout time (they can contain
    // repeat()/minmax() which need the container width):
    //   "15.5rem minmax(0,1fr)"  /  "repeat(auto-fill, minmax(310px,1fr))"
    std::string gridTemplateColumns, gridTemplateRows;
    bool hasGridTemplateColumns = false, hasGridTemplateRows = false;
    // grid-template-areas: raw quoted rows, e.g. "'a b' 'c d'".
    std::string gridTemplateAreas;
    bool hasGridTemplateAreas = false;
    // grid-area on a child: named area ("columnStart") or "auto".
    std::string gridArea;
    bool hasGridArea = false;
    // grid-column / grid-row on a child — line-based placement used by
    // virtually every modern grid site ("grid-column: 3 / span 9").
    // startLine: 1-based grid line, 0 = auto. endLine: grid line, 0 =
    // unset, negative = counted from the end (-1 = last line). span:
    // track count when written as "span N", 0 = auto/1.
    int gridColStartLine = 0, gridColEndLine = 0, gridColSpan = 0;
    int gridRowStartLine = 0, gridRowEndLine = 0, gridRowSpan = 0;
    bool hasGridColumn = false, hasGridRow = false;

    bool hasColor = false;
    bool hasBg    = false;
    bool hasFontSize = false;
    bool hasBold = false;
    bool hasItalic = false;
    bool hasUnderline = false;
    bool hasMargin = false;
    bool hasPadding = false;
    bool hasBorder = false;
    bool hasBorderColor = false;

    // Bitmask of properties the current declaration list actually set.
    uint64_t declared = 0;
};

// Merge `src`'s declared properties (src.declared bits) into `dst`,
// leaving everything else untouched. Used by the cascade.
void mergeDeclared(Style& dst, const Style& src);

Style applyStyle(const Style& base, const std::string& css);

// One matched CSS rule. `specificity` is the standard (ids,classes,elements)
// triple packed into a single int: ids*10000 + classes*100 + elements.
// Rules are sorted by specificity at parse time, so later rules in the
// vector always win during application. `media` holds a parsed @media
// condition; empty = unconditional.
// Pre-parsed attribute check: "[attr]", "[attr=v]", "[attr^=v]" etc.,
// split once at parse time so matching never allocates.
struct AttrCheck {
    std::string name;   // lowercased attribute name
    char op;            // 0 (presence), '=', '^', '$', '*', '~', '|'
    std::string value;  // raw value (case-sensitive per CSS)
};

// Pre-parsed "simple selector" — one compound selector with no
// combinators, e.g. "div.foo#bar[title]:hover". Static parts (tag/id/
// classes/attrs) are matched with plain compares; pseudo-classes keep
// their name (lowercased) and raw argument for runtime evaluation.
struct SimpleSel {
    std::string tag = "";               // "" none, "*" universal
    std::string id = "";
    std::vector<std::string> classes;
    std::vector<AttrCheck> attrs;
    struct Pseudo { std::string name; std::string arg; };
    std::vector<Pseudo> pseudos;
};

// One item of a full selector: either a combinator or a compound.
struct SelItem {
    bool isComb = false;
    char comb = 0;        // ' ', '>', '+', '~'
    SimpleSel simple;
};

struct SelectorKey {
    char kind = 0;   // 0 = candidate for every node (universal/pseudo),
                     // 't' = tag, 'c' = class, 'i' = id,
                     // 'a' = attribute name, 'h' = interactive pseudo
                     // (:hover/:focus/...) of the rightmost compound
    std::string key; // lowercase tag / class / id / attr name when kind
};

struct CSSRule {
    std::vector<std::string> selectors;
    std::vector<int> specificities;   // parallel to selectors
    Style style;
    std::string media;                // raw condition, "" = always

    // Precomputed matching aids (parallel to selectors), filled by
    // parseCSS. They let computeStyle skip the full (allocating) selector
    // match for the vast majority of node/rule pairs, which is what made
    // big real-world stylesheets take seconds per layout pass.
    std::vector<std::vector<std::string>> selTokens;
    std::vector<SelectorKey> selKeys;
    // Fully pre-parsed selectors (per selector: compound/combinator
    // sequence). Matching runs allocation-free on these.
    std::vector<std::vector<SelItem>> selItems;
};

std::vector<CSSRule> parseCSS(const std::string& css);

// Monotonic counter bumped by every parseCSS call. Consumers that cache
// derived data from a rules vector (layout's rule buckets) use it to
// detect that a NEW ruleset arrived, even when the vector happens to sit
// at the same address with the same size after re-assignment.
uint64_t parseCSSGeneration();

// Set the viewport width used to evaluate @media (min-/max-width)
// conditions. Called by the layout before style resolution.
void setMediaViewportWidth(int w);
int mediaViewportWidth();

// Evaluate a raw @media condition string (e.g. "screen and (max-width:
// 768px)") against the current media viewport width.
bool mediaConditionMatches(const std::string& cond);

// Global page zoom. CSS font sizes (and tag defaults like h1) are
// multiplied by this inside computeStyle, so zoom now affects EVERYTHING
// including headings, not just rules that declare font-size.
void setGlobalZoom(float z);
float getGlobalZoom();

// Register CSS custom properties (--name: value) for var() resolution.
// Called by parseCSS; exposed for tests.
void setCustomProperty(const std::string& name, const std::string& value);
std::string getCustomProperty(const std::string& name);
void clearCustomProperties();

// Simple single-selector match (no combinators). Back-compat for tests.
bool matchesSelector(const std::string& selector,
                     const std::string& tag,
                     const std::map<std::string, std::string>& attrs);

// Compound selector match. Walks the node's ancestors to evaluate
// descendant ("a b"), child ("a > b"), and adjacent-sibling ("a + b")
// combinators. Returns true if `selector` matches `node`.
bool matchesSelectorNode(const std::string& selector,
                         const std::shared_ptr<Node>& node);

// Like matchesSelectorNode, but also handles `:hover` — true if the
// selector matches the node AND (if `:hover` appears) the node is the
// currently hovered node. Pass nullptr for hovered if no hover.
bool matchesSelectorNodeHover(const std::string& selector,
                               const std::shared_ptr<Node>& node,
                               const std::shared_ptr<Node>& hovered);

// Token-based variant: same result as matchesSelectorNodeHover but
// without re-tokenizing the selector string on every call. Callers that
// match the same selector against many nodes (computeStyle) use this
// with the tokens cached on the rule.
bool matchesSelectorTokens(const std::vector<std::string>& toks,
                           const std::shared_ptr<Node>& node,
                           const std::shared_ptr<Node>& hovered);

// Fastest variant: match a fully pre-parsed selector (CSSRule::selItems).
// The hot path performs no string parsing or allocation at all.
bool matchesSelectorItems(const std::vector<SelItem>& items,
                          const std::shared_ptr<Node>& node,
                          const std::shared_ptr<Node>& hovered);

// Clears the ancestor-fingerprint cache used to prune descendant
// selectors. Must be called whenever the DOM or the pass scope changes
// (the layout calls it at the start of every layout()).
void clearSelectorAncCache();

} // namespace browser