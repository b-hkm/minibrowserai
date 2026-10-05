#include "font_loader.h"
#include <iostream>
#include <map>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace browser {

static std::string g_fontPath;
static std::map<std::tuple<int, bool, bool>, TTF_Font*> g_cache;

static const char* kSansPaths[] = {
    "DejaVuSans.ttf", "./DejaVuSans.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
    "/Library/Fonts/Arial.ttf",
    "C:/Windows/Fonts/arial.ttf",
    nullptr
};

static const char* kMonoPaths[] = {
    "DejaVuSansMono.ttf", "./DejaVuSansMono.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
    "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
    "/usr/share/fonts/dejavu/DejaVuSansMono.ttf",
    "C:/Windows/Fonts/consola.ttf",
    nullptr
};

static const char* kSerifPaths[] = {
    "DejaVuSerif.ttf", "./DejaVuSerif.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSerif.ttf",
    "/usr/share/fonts/TTF/DejaVuSerif.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSerif-Regular.ttf",
    "C:/Windows/Fonts/georgia.ttf",
    nullptr
};

static const char** pathsForFamily(const std::string& family) {
    if (family == "monospace") return kMonoPaths;
    if (family == "serif")     return kSerifPaths;
    return kSansPaths;  // sans-serif default
}

// Fallback font covering CJK (and wide Unicode) ranges. Used when a run
// contains characters the primary font cannot render (Chinese/Japanese/
// Korean text on otherwise-Latin pages).
// Static (non-variable) CJK fonts only: this SDL_ttf/FreeType build
// rasterizes NotoSansSC[wght].ttf as a completely BLANK surface (all
// glyphs come out transparent), so variable fonts must not be used.
static const char* kCJKPaths[] = {
    "NotoSansSC-Regular.ttf", "./NotoSansSC-Regular.ttf",
    "/usr/share/fonts/truetype/chinese/NotoSansSC-Regular.ttf",
    "/usr/share/fonts/truetype/noto-serif-sc/NotoSerifSC-Regular.ttf",
    "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
    "/usr/share/fonts/truetype/chinese/SarasaMonoSC-Regular.ttf",
    "/usr/share/fonts/truetype/lxgw-wenkai/LXGWWenKai-Regular.ttf",
    "/usr/share/fonts/truetype/noto/NotoSansSC-Regular.ttf",
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    "C:/Windows/Fonts/msyh.ttc",
    nullptr
};

static bool cpNeedsCJK(uint32_t cp) {
    return (cp >= 0x2E80 && cp <= 0x9FFF) ||   // CJK radicals..unified
           (cp >= 0x3040 && cp <= 0x30FF) ||   // kana
           (cp >= 0x3130 && cp <= 0x318F) ||   // Hangul compat
           (cp >= 0x3400 && cp <= 0x4DBF) ||   // CJK ext A
           (cp >= 0xAC00 && cp <= 0xD7AF) ||   // Hangul
           (cp >= 0xF900 && cp <= 0xFAFF) ||   // CJK compat
           (cp >= 0xFF00 && cp <= 0xFF60);     // fullwidth forms
}

bool textNeedsCJK(const std::string& utf8) {
    for (size_t i = 0; i < utf8.size();) {
        unsigned char c = (unsigned char)utf8[i];
        if (c < 0x80) { ++i; continue; }
        int len = 0; uint32_t cp = 0;
        if ((c & 0xE0) == 0xC0) { len = 2; cp = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07; }
        else { ++i; continue; }
        if (i + (size_t)len > utf8.size()) break;
        ++i;   // past the lead byte; continuation bytes follow
        bool ok = true;
        for (int k = 1; k < len; ++k, ++i) {
            unsigned char cc = (unsigned char)utf8[i];
            if ((cc & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (ok && cpNeedsCJK(cp)) return true;
    }
    return false;
}

TTF_Font* getCJKFont(int ptSize, bool bold, bool italic) {
    static std::map<std::tuple<int, bool, bool>, TTF_Font*> cjkCache;
    auto key = std::make_tuple(ptSize, bold, italic);
    auto it = cjkCache.find(key);
    if (it != cjkCache.end()) return it->second;
    for (int i = 0; kCJKPaths[i]; ++i) {
        FILE* test = fopen(kCJKPaths[i], "rb");
        if (!test) continue;
        fclose(test);
        if (TTF_Font* f = TTF_OpenFont(kCJKPaths[i], ptSize)) {
            int style = TTF_STYLE_NORMAL;
            if (bold)   style |= TTF_STYLE_BOLD;
            if (italic) style |= TTF_STYLE_ITALIC;
            TTF_SetFontStyle(f, style);
            cjkCache[key] = f;
            return f;
        }
    }
    cjkCache[key] = nullptr;
    return nullptr;
}

TTF_Font* loadFont(const std::string& pathHint, int ptSize) {
    auto tryOpen = [&](const char* p) -> TTF_Font* {
        if (!p || !*p) return nullptr;
        TTF_Font* f = TTF_OpenFont(p, ptSize);
        if (f) g_fontPath = p;
        return f;
    };
    if (!pathHint.empty()) {
        if (auto* f = tryOpen(pathHint.c_str())) return f;
    }
    // Default opener: sans-serif at the original API
    for (int i = 0; kSansPaths[i]; ++i) {
        if (auto* f = tryOpen(kSansPaths[i])) return f;
    }
    return nullptr;
}

static TTF_Font* openForFamily(const std::string& family, int ptSize) {
    const char** paths = pathsForFamily(family);
    for (int i = 0; paths[i]; ++i) {
        // Test if file is accessible
        FILE* test = fopen(paths[i], "rb");
        if (!test) {
            // Font file not found at this path — try the next one.
            // (Suppressed the per-path "[font-miss]" message to avoid
            // spamming stderr on every font load attempt.)
            continue;
        }
        fclose(test);
        TTF_Font* f = TTF_OpenFont(paths[i], ptSize);
        if (f) {
            g_fontPath = paths[i];
            return f;
        }
    }
    // Fallback to sans.
    for (int i = 0; kSansPaths[i]; ++i) {
        TTF_Font* f = TTF_OpenFont(kSansPaths[i], ptSize);
        if (f) {
            g_fontPath = kSansPaths[i];
            return f;
        }
    }
    return nullptr;
}

TTF_Font* getSizedFont(int ptSize, bool bold, bool italic) {
    auto key = std::make_tuple(ptSize, bold, italic);
    auto it = g_cache.find(key);
    if (it != g_cache.end()) return it->second;

    TTF_Font* f = loadFont(g_fontPath, ptSize);
    if (!f) return nullptr;
    int style = TTF_STYLE_NORMAL;
    if (bold)   style |= TTF_STYLE_BOLD;
    if (italic) style |= TTF_STYLE_ITALIC;
    TTF_SetFontStyle(f, style);
    g_cache[key] = f;
    return f;
}

TTF_Font* getFontForFamily(const std::string& family,
                           int ptSize, bool bold, bool italic) {
    // key: family + size + bold + italic
    struct Key {
        std::string family;
        int size; bool bold; bool italic;
        bool operator<(const Key& o) const {
            if (family != o.family) return family < o.family;
            if (size != o.size) return size < o.size;
            if (bold != o.bold) return bold < o.bold;
            return italic < o.italic;
        }
    };
    static std::map<Key, TTF_Font*> familyCache;
    Key k{family, ptSize, bold, italic};
    auto it = familyCache.find(k);
    if (it != familyCache.end()) return it->second;

    TTF_Font* f = openForFamily(family, ptSize);
    if (!f) return nullptr;
    int style = TTF_STYLE_NORMAL;
    if (bold)   style |= TTF_STYLE_BOLD;
    if (italic) style |= TTF_STYLE_ITALIC;
    TTF_SetFontStyle(f, style);
    familyCache[k] = f;
    return f;
}

void shutdownFonts() {
    clearMeasureCache();
    for (auto& [k, f] : g_cache) {
        (void)k;
        if (f) TTF_CloseFont(f);
    }
    g_cache.clear();
    g_fontPath.clear();
    // NOTE: familyCache / cjkCache fonts are intentionally leaked here —
    // they are process-lifetime and shutdown happens at exit.
}

const std::string& resolvedFontPath() { return g_fontPath; }

// Run-level font picking shared by layout measurement and rendering:
// CJK text switches to the CJK-capable font (Latin fonts render .notdef
// boxes there); otherwise family/size/style resolution as before. Both
// sides call this with the same arguments, so measurements match drawing.
TTF_Font* getFontForRun(const std::string& text, const std::string& family,
                        int ptSize, bool bold, bool italic,
                        TTF_Font* defaultFont) {
    if (getenv("MB_FONTDBG") && textNeedsCJK(text))
        fprintf(stderr, "[font] CJK text detected: '%s'\n",
                text.substr(0, 30).c_str());
    if (textNeedsCJK(text)) {
        TTF_Font* cjk = getCJKFont(ptSize, bold, italic);
        if (getenv("MB_FONTDBG")) fprintf(stderr, "[font] cjk=%p\n", (void*)cjk);
        if (cjk) return cjk;
    }
    bool nonDefault = (family != "sans-serif") || (ptSize != 16) ||
                      italic || bold;
    if (nonDefault) {
        if (TTF_Font* f = getFontForFamily(family, ptSize, bold, italic))
            return f;
    }
    // Callers that have no font of their own (wrapRuns measures with a
    // null defaultFont) must still get a REAL font: a null here made
    // TTF_SizeUTF8 measure every word at 0px, so default-styled (16px
    // regular) text never wrapped and painted over floats.
    if (defaultFont) return defaultFont;
    return getSizedFont(ptSize, bold, italic);
}

// ---------------------------------------------------------------------------
// Text measurement cache
//
// A relayout re-measures every text run (word-wrapping calls TTF_SizeUTF8
// per word, and splitRun-style paths measure prefixes character by
// character). A Wikipedia-sized page measures ~10k+ strings; with the
// cache, repeat relayouts (image arrivals, JS DOM edits, zoom changes)
// skip nearly all of them. Bounded: on overflow the whole cache drops and
// rebuilds lazily — the same policy as the text-texture cache.
// ---------------------------------------------------------------------------
struct MeasureKey {
    TTF_Font* font;
    std::string text;
    bool operator==(const MeasureKey& o) const {
        return font == o.font && text == o.text;
    }
};
struct MeasureKeyHash {
    size_t operator()(const MeasureKey& k) const {
        size_t h = std::hash<void*>()(k.font);
        h ^= std::hash<std::string>()(k.text) + 0x9e3779b97f4a7c15ull
           + (h << 6) + (h >> 2);
        return h;
    }
};
struct MeasureVal { int w, h; };
static std::unordered_map<MeasureKey, MeasureVal, MeasureKeyHash> g_measure;
// v2.18: increased from 60,000 to 200,000. The measure cache stores
// (font, text) → (width, height) pairs so the layout engine doesn't
// call TTF_SizeUTF8 for repeated words. At 60,000 entries, text-heavy
// pages (Wikipedia, GitHub) with many unique words across multiple
// font sizes could overflow the cache and clear ALL entries — causing
// every word measurement to be a cache miss on the next layout pass.
// At 200,000 the cache survives most pages without overflow.
static const size_t kMeasureCap = 200000;

bool measureTextCached(TTF_Font* font, const std::string& text, int& w, int& h) {
    w = h = 0;
    if (!font || text.empty()) return false;
    MeasureKey key{font, text};
    auto it = g_measure.find(key);
    if (it != g_measure.end()) {
        w = it->second.w;
        h = it->second.h;
        return true;
    }
    int mw = 0, mh = 0;
    if (TTF_SizeUTF8(font, text.c_str(), &mw, &mh) != 0) return false;
    if (g_measure.size() >= kMeasureCap) g_measure.clear();
    g_measure.emplace(std::move(key), MeasureVal{mw, mh});
    w = mw;
    h = mh;
    return true;
}

void clearMeasureCache() { g_measure.clear(); }

} // namespace browser