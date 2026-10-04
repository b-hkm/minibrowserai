#pragma once
#include "../css/style.h"
#include "../html/parser.h"
#include "resource.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <string>
#include <vector>

namespace browser {

struct Run {
    std::string text;
    // Original (logical-order) text when bidi/shaping changed `text`
    // into visual order. Empty when identical. Find-in-page, link
    // labels and caret math should prefer this when non-empty.
    std::string logical;
    Style style;
    bool isLink = false;
    std::string href;
    std::weak_ptr<Node> linkNode;  // the <a> this run lives inside (if any)
    // The element that produced this run: the nearest element ancestor
    // for text runs, the <img>/<br> itself for atomic/break runs. Lets
    // rectForNode() compute fragment unions for inline elements
    // (<b>/<i>/<span>/...) that own no box of their own.
    std::weak_ptr<Node> sourceNode;

    // Inline image run (text empty). The renderer draws it at the run's
    // position inside the line; the wrapper treats it as an atomic box.
    bool isImage = false;
    std::string imagePath;   // resolved src/URL/data-uri for the renderer
    int drawW = 0, drawH = 0;
    std::string altText;
};

struct Box {
    int x = 0, y = 0, w = 0, h = 0;
    Style style;
    std::vector<std::vector<Run>> lines;
    // Per-line heights (parallel to lines). Empty = uniform fontSize+6.
    std::vector<int> lineHeights;
    // Per-line float insets (parallel to lines): .first = left inset,
    // .second = right inset (both relative to the box's content edge).
    // Lines that flow beside a floated image must start (LTR) / end
    // (RTL) inside the shrunken span, which the renderer applies here.
    std::vector<std::pair<int,int>> lineInsets;
    // Base text direction of this box's content (dir attribute / CSS
    // direction / dir=auto detection). RTL lines are stored in visual
    // run order and right-aligned by the renderer.
    bool rtl = false;

    bool isImage = false;
    std::string imagePath;
    int imageW = 0, imageH = 0;
    int drawW = 0, drawH = 0;
    std::string altText;
    // Internal media element (<video>/<audio>). The renderer draws the
    // decoded frame (media::MediaPlayer, keyed by mediaPath) plus a
    // Chrome-style control bar; browser.cpp hit-tests clicks through
    // renderer::mediaHitTest so drawing and input never disagree.
    bool isVideo = false, isAudio = false;
    std::string mediaPath;    // resolved src (registry key)
    std::string posterPath;   // resolved poster attribute ("" = none)
    // CSS background-image (resolved path or remote URL); empty = none.
    std::string bgImagePath;
    std::weak_ptr<Node> sourceNode;  // the DOM node this box represents
};

struct Link {
    int x = 0, y = 0, w = 0, h = 0;
    std::string href;
    std::string displayText;
    std::weak_ptr<Node> sourceNode;  // the <a> node this hit-rect belongs to
};

struct LayoutResult {
    std::vector<Box> boxes;
    std::vector<Link> links;
    int contentHeight = 0;
};

// ---------------------------------------------------------------------------
// Text selection support (round 3)
// ---------------------------------------------------------------------------

// A position in the laid-out text: which box, which line, and the byte
// offset within that line's concatenated run text. `byte` always lands
// on a UTF-8 codepoint boundary.
struct TextPos {
    int box = -1, line = -1, byte = 0;
    bool valid = false;
    bool operator==(const TextPos& o) const {
        return valid == o.valid && box == o.box && line == o.line &&
               byte == o.byte;
    }
    bool operator<(const TextPos& o) const {
        if (box != o.box) return box < o.box;
        if (line != o.line) return line < o.line;
        return byte < o.byte;
    }
};

// Map a point in DOCUMENT coordinates (box space: x as-is, y including
// scroll) to the nearest text position. Falls back to the closest line
// within a small vertical margin so clicks just past the last line snap
// to the end of the text.
TextPos hitTestText(const LayoutResult& lr, TTF_Font* defaultFont,
                    int x, int y);

// Number of bytes in the line's concatenated run text.
int lineByteLength(const Box& b, int lineIdx);

// Text between two positions (start inclusive, end exclusive), in
// document order regardless of the anchor/focus orientation. Runs that
// were bidi-reordered contribute their `logical` text when set.
std::string textBetween(const LayoutResult& lr, const TextPos& a,
                        const TextPos& b);

// baseDir is the directory of the HTML file (or the page URL for remote
// pages), used to resolve relative image URLs.
LayoutResult layout(const std::shared_ptr<Node>& root,
                    int width, TTF_Font* font,
                    const std::vector<CSSRule>& cssRules = {},
                    const std::string& baseDir = "");

// ---------------------------------------------------------------------------
// Element geometry (round 4) — getBoundingClientRect support
// ---------------------------------------------------------------------------

// Union of every laid-out fragment that belongs to `node`, in DOCUMENT
// coordinates (CSS pixels):
//   - the node's own box     (blocks, images, form widgets)
//   - the node's link rects  (per-line <a> hit-test fragments)
//   - the node's run fragments (inline <b>/<i>/<span>/... text and
//     inline images inside a containing block's lines), measured with
//     the exact geometry the renderer and hitTestText use (font
//     resolution, text-align, float insets, line heights).
// Returns false when the node has no laid-out fragments (detached,
// display:none, empty inline element); `out` is left all-zero then.
bool rectForNode(const LayoutResult& lr, TTF_Font* defaultFont,
                 const std::shared_ptr<Node>& node, DOMRect& out);

// Set the currently-hovered node so CSS `:hover` selectors match it.
// Pass nullptr to clear hover. The pointer is held by a static inside
// layout.cpp, so call this before each layout() call.
void setHoveredNode(const std::shared_ptr<Node>& node);

} // namespace browser
