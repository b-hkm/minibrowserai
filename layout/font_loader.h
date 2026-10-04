#pragma once
#include <SDL2/SDL_ttf.h>
#include <string>

namespace browser {

TTF_Font* loadFont(const std::string& pathHint = "", int ptSize = 16);
TTF_Font* getSizedFont(int ptSize, bool bold = false, bool italic = false);
// Family-aware lookup. family should be one of:
//   "monospace"   -> loads DejaVuSansMono.ttf
//   "serif"       -> loads DejaVuSerif.ttf
//   "sans-serif"  -> loads DejaVuSans.ttf
//   any other     -> falls back to sans-serif
TTF_Font* getFontForFamily(const std::string& family,
                           int ptSize, bool bold = false, bool italic = false);
// True when the UTF-8 text contains CJK codepoints (which the primary
// Latin fonts cannot render — they'd show as .notdef boxes).
bool textNeedsCJK(const std::string& utf8);
// Font covering the CJK ranges (Noto Sans SC where available). Cached.
TTF_Font* getCJKFont(int ptSize, bool bold = false, bool italic = false);
// Shared run-level font choice: CJK fallback first, then family resolution.
// Layout measurement and the renderer MUST both use this so widths match.
TTF_Font* getFontForRun(const std::string& text, const std::string& family,
                        int ptSize, bool bold, bool italic,
                        TTF_Font* defaultFont);

// TTF_SizeUTF8 with a (font, text) -> width cache. Layout measures the
// same words over and over (every relayout re-flows every run; scrolling
// never relayouts but zoom/JS/image arrivals do). The cache makes repeat
// relayouts — the common case — nearly free on weak devices. Both out
// params are always set; empty text measures 0x0.
bool measureTextCached(TTF_Font* font, const std::string& text, int& w, int& h);

// Free the measure cache (call with shutdownFonts()).
void clearMeasureCache();

void shutdownFonts();
const std::string& resolvedFontPath();


} // namespace browser
