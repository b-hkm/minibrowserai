#pragma once
// Text shaping + bidirectional (bidi) support for right-to-left scripts.
//
// SDL_ttf renders codepoint-by-codepoint in string order, which mangles
// Arabic in two ways: letters render in their isolated forms disconnected
// from each other, and the whole run flows left-to-right (Arabic reads
// right-to-left). This module fixes both without new dependencies:
//
//  * Arabic shaping: letters are mapped to their Unicode "presentation
//    forms" (U+FE70..FEFC) according to the standard joining rules,
//    including lam-alef ligatures. DejaVu Sans (the default font) and
//    FreeSerif carry the full set of presentation-form glyphs, so the
//    existing TTF_RenderUTF8 path renders them as connected script.
//  * Bidi reordering: a simplified UAX#9 (single embedding level, which
//    covers real-world page content) splits each laid-out line into
//    LTR / RTL segments, resolves neutrals and numbers, and emits runs
//    in VISUAL order so the renderer can keep drawing left-to-right.

#include <string>
#include <vector>

namespace browser {

struct Run;  // defined in layout.h

// True if the UTF-8 string contains any strong RTL character (Arabic,
// Hebrew, or RTL presentation forms). Pure-ASCII input exits immediately.
bool containsRTL(const std::string& utf8);

// UAX#9 rules P2/P3: true if the first strong character of the text is
// RTL ("dir=auto" / first-strong direction detection).
bool detectRTL(const std::string& utf8);

// Shape one logical RTL segment into a display-ready UTF-8 string:
// contextual presentation forms, lam-alef ligatures, digit clusters kept
// forward ("2024" stays "2024"), combining marks attached before their
// base, bracket pairs mirrored. The result is the visual string — draw
// it with the normal LTR text renderer.
std::string shapeRTLLogical(const std::string& utf8);

// Reorder one laid-out line of runs (LOGICAL order) into VISUAL order:
// splits runs at direction boundaries, shapes the RTL parts, and — for
// an RTL base direction — reverses the segment order. Image runs take
// part in the reordering as atomic units. Runs whose visual text differs
// from the logical text keep the original substring in Run::logical
// (used by find-in-page and link labels).
void applyBidiToLine(std::vector<Run>& line, bool baseRTL);

} // namespace browser
