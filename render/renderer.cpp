#include "renderer.h"
#include "../layout/font_loader.h"
#include "../layout/resource.h"
#include "../media/mediaplayer.h"
#include <algorithm>
#include <iostream>
#include <unordered_map>

namespace browser {

// Defined below: streaming textures for the internal media players.
void clearMediaTextures();

// Per-renderer texture cache for images. The previous code created a
// fresh SDL_Texture from the SDL_Surface on EVERY frame for EVERY image,
// then destroyed it immediately — that's the slowest possible path.
// We now cache the texture keyed by the resolved image path. The cache
// is cleared by shutdownImages() so it doesn't outlive the renderer.
static std::unordered_map<std::string, SDL_Texture*> g_textureCache;

// ---------------------------------------------------------------------------
// v2.19: GLYPH ATLAS — per-character text rendering
//
// The v2.18 approach cached each unique LINE of text as one texture
// (cachedTextTexture). For text-heavy pages (Wikipedia articles), each
// line is a unique string → cache miss → TTF_RenderUTF8_Blended (1-5ms
// per line). 100 unique lines = 100-500ms spike on first render.
//
// The glyph atlas renders each CHARACTER once as a small white texture
// (~10x20px). Text is then composed by blitting individual glyphs with
// SDL_SetTextureColorMod for coloring. This is O(unique_chars) creation
// (~100 ASCII chars × 0.5ms = 50ms one-time) instead of O(unique_lines)
// (100+ lines × 5ms = 500ms+). Subsequent renders are pure SDL_RenderCopy
// blits — no TTF_RenderUTF8_Blended at all.
//
// The approach is identical to how Chrome (DirectWrite glyph cache),
// Firefox (WebRender glyph atlas), and SDL2 game engines handle text.
// ---------------------------------------------------------------------------

struct GlyphKey {
    TTF_Font* font;
    uint32_t  codepoint;
    bool operator==(const GlyphKey& o) const {
        return font == o.font && codepoint == o.codepoint;
    }
};
struct GlyphKeyHash {
    size_t operator()(const GlyphKey& k) const {
        return std::hash<TTF_Font*>()(k.font) ^
               (std::hash<uint32_t>()(k.codepoint) << 1);
    }
};
struct GlyphEntry {
    SDL_Texture* tex = nullptr;
    int w = 0, h = 0;  // advance width + height
};
static std::unordered_map<GlyphKey, GlyphEntry, GlyphKeyHash> g_glyphCache;
static const size_t kGlyphCacheCap = 5000;

// Get (or create) a cached glyph texture for a single Unicode codepoint.
// The glyph is rendered in WHITE so SDL_SetTextureColorMod can tint it
// to any color at draw time (no per-color cache entries needed).
static GlyphEntry* getGlyph(SDL_Renderer* ren, TTF_Font* font,
                            uint32_t cp) {
    GlyphKey key{font, cp};
    auto it = g_glyphCache.find(key);
    if (it != g_glyphCache.end()) return &it->second;

    // Encode the codepoint as UTF-8 for SDL_ttf.
    char utf8[5] = {};
    if (cp < 0x80) {
        utf8[0] = (char)cp;
    } else if (cp < 0x800) {
        utf8[0] = (char)(0xC0 | (cp >> 6));
        utf8[1] = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        utf8[0] = (char)(0xE0 | (cp >> 12));
        utf8[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        utf8[2] = (char)(0x80 | (cp & 0x3F));
    } else {
        utf8[0] = (char)(0xF0 | (cp >> 18));
        utf8[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        utf8[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        utf8[3] = (char)(0x80 | (cp & 0x3F));
    }

    // Render the glyph in white (255,255,255,255). The alpha channel
    // carries the anti-aliasing; SDL_SetTextureColorMod replaces the
    // white with the desired text color at draw time.
    SDL_Color white = {255, 255, 255, 255};
    SDL_Surface* surf = TTF_RenderUTF8_Blended(font, utf8, white);
    if (!surf) return nullptr;

    GlyphEntry ent;
    ent.w = surf->w;
    ent.h = surf->h;
    ent.tex = SDL_CreateTextureFromSurface(ren, surf);
    SDL_FreeSurface(surf);
    if (!ent.tex) return nullptr;

    // Set the texture's blend mode to blend (alpha blending) so the
    // glyph's anti-aliased edges composite correctly over backgrounds.
    SDL_SetTextureBlendMode(ent.tex, SDL_BLENDMODE_BLEND);

    if (g_glyphCache.size() >= kGlyphCacheCap) {
        // Evict a quarter of the cache (not all — keeps it warm).
        size_t toEvict = kGlyphCacheCap / 4;
        auto it2 = g_glyphCache.begin();
        for (size_t i = 0; i < toEvict && it2 != g_glyphCache.end(); ++i) {
            if (it2->second.tex) SDL_DestroyTexture(it2->second.tex);
            it2 = g_glyphCache.erase(it2);
        }
    }
    auto [inserted, _] = g_glyphCache.emplace(std::move(key), ent);
    return &inserted->second;
}

// Draw a text run using the glyph atlas. Falls back to the line-level
// cachedTextTexture for very short runs (1-3 chars) where the per-glyph
// overhead doesn't pay off.
static void drawTextWithGlyphs(SDL_Renderer* ren, TTF_Font* font,
                               const std::string& utf8,
                               SDL_Color color, int x, int y,
                               int& outW, int& outH) {
    outW = outH = 0;
    if (!font || utf8.empty()) return;

    // Set the color modulation ONCE per run — all glyphs in this run
    // share the same color. SDL_SetTextureColorMod multiplies the
    // texture's RGB by this color; the glyph's alpha (anti-aliasing)
    // is preserved.
    // SDL_SetTextureColorMod is per-texture, so we set it inside the
    // loop for each glyph (the glyph textures are shared across runs
    // with different colors).

    int curX = x;
    int maxH = 0;
    const unsigned char* s = (const unsigned char*)utf8.c_str();
    size_t i = 0;
    size_t len = utf8.size();
    while (i < len) {
        uint32_t cp = 0;
        if (s[i] < 0x80) {
            cp = s[i];
            i += 1;
        } else if (s[i] < 0xC0) {
            // Invalid continuation byte — skip.
            i += 1;
            continue;
        } else if (s[i] < 0xE0) {
            if (i + 1 >= len) break;
            cp = ((s[i] & 0x1F) << 6) | (s[i+1] & 0x3F);
            i += 2;
        } else if (s[i] < 0xF0) {
            if (i + 2 >= len) break;
            cp = ((s[i] & 0x0F) << 12) | ((s[i+1] & 0x3F) << 6) | (s[i+2] & 0x3F);
            i += 3;
        } else {
            if (i + 3 >= len) break;
            cp = ((s[i] & 0x07) << 18) | ((s[i+1] & 0x3F) << 12) |
                 ((s[i+2] & 0x3F) << 6) | (s[i+3] & 0x3F);
            i += 4;
        }

        // Space (U+0020): no glyph to draw, but advance the cursor.
        if (cp == ' ') {
            int sw = 0, sh = 0;
            measureTextCached(font, " ", sw, sh);
            curX += sw;
            maxH = std::max(maxH, sh);
            continue;
        }

        GlyphEntry* g = getGlyph(ren, font, cp);
        if (!g || !g->tex) continue;
        // Tint the white glyph to the desired color.
        SDL_SetTextureColorMod(g->tex, color.r, color.g, color.b);
        SDL_Rect dst = {curX, y, g->w, g->h};
        SDL_RenderCopy(ren, g->tex, nullptr, &dst);
        curX += g->w;
        maxH = std::max(maxH, g->h);
    }
    outW = curX - x;
    outH = maxH;
}

void clearGlyphCache() {
    for (auto& [k, v] : g_glyphCache)
        if (v.tex) SDL_DestroyTexture(v.tex);
    g_glyphCache.clear();
}

// ---------------------------------------------------------------------------
// Text texture cache
//
// Every text run used to be rasterized and uploaded per frame:
// TTF_RenderUTF8_Blended -> SDL_CreateTextureFromSurface -> draw -> destroy.
// A Wikipedia-sized page has ~2000 runs, so a single frame burned minutes
// of CPU on repeat visits. Text on a laid-out page is immutable (except
// hover recoloring and typed input), so (font, color, string) is a perfect
// cache key. Bounded: when the entry count exceeds the cap the whole cache
// is dropped and rebuilt lazily — cheap, since it only happens after huge
// page changes.
// ---------------------------------------------------------------------------
struct TextKey {
    TTF_Font* font;
    SDL_Color color;
    std::string text;
    bool operator==(const TextKey& o) const {
        return font == o.font && color.r == o.color.r && color.g == o.color.g
            && color.b == o.color.b && color.a == o.color.a && text == o.text;
    }
};
struct TextKeyHash {
    size_t operator()(const TextKey& k) const {
        size_t h = std::hash<void*>()(k.font);
        h ^= std::hash<uint8_t>()(k.color.r) + 0x9e3779b97f4a7c15ull
           + (h << 6) + (h >> 2);
        h ^= std::hash<uint8_t>()(k.color.g) + 0x9e3779b97f4a7c15ull
           + (h << 6) + (h >> 2);
        h ^= std::hash<uint8_t>()(k.color.b) + 0x9e3779b97f4a7c15ull
           + (h << 6) + (h >> 2);
        h ^= std::hash<std::string>()(k.text) + 0x9e3779b97f4a7c15ull
           + (h << 6) + (h >> 2);
        return h;
    }
};
struct TextTex { SDL_Texture* tex; int w, h; };
static std::unordered_map<TextKey, TextTex, TextKeyHash> g_textCache;
// v2.18: increased from 12,000 to 50,000. Text-heavy pages (Wikipedia
// articles, DDG search results with snippets, GitHub readmes) can have
// 500+ unique text runs (each sentence/word is a separate texture
// because of font/color variations). At 12,000 the cache overflowed
// and CLEARED ALL entries — causing a 100-500ms spike on the next
// render as every text run became a cache miss (TTF_RenderUTF8_Blended
// is 1-5ms per call). At 50,000 the cache survives most pages without
// overflow, so scrolling is smooth (cache hits only).
static const size_t kTextCacheCap = 50000;

SDL_Texture* cachedTextTexture(SDL_Renderer* ren, TTF_Font* font,
                               const std::string& utf8, SDL_Color color,
                               int* w, int* h) {
    if (!font || utf8.empty()) { if (w) *w = 0; if (h) *h = 0; return nullptr; }
    TextKey key{font, color, utf8};
    auto it = g_textCache.find(key);
    if (it != g_textCache.end()) {
        if (w) *w = it->second.w;
        if (h) *h = it->second.h;
        return it->second.tex;
    }
    SDL_Surface* surf = TTF_RenderUTF8_Blended(font, utf8.c_str(), color);
    if (!surf) {
        if (getenv("MB_FONTDBG"))
            std::fprintf(stderr, "[tex] render FAILED font=%p err=%s"
                                 " text='%.40s'\n",
                         (void*)font, TTF_GetError(), utf8.c_str());
        if (w) *w = 0;
        if (h) *h = 0;
        return nullptr;
    }
    TextTex ent{nullptr, surf->w, surf->h};
    ent.tex = SDL_CreateTextureFromSurface(ren, surf);
    SDL_FreeSurface(surf);
    if (!ent.tex) {
        if (w) *w = 0;
        if (h) *h = 0;
        return nullptr;
    }
    if (g_textCache.size() >= kTextCacheCap) clearTextTextureCache();
    g_textCache.emplace(std::move(key), ent);
    if (w) *w = ent.w;
    if (h) *h = ent.h;
    return ent.tex;
}

void clearTextTextureCache() {
    for (auto& kv : g_textCache)
        if (kv.second.tex) SDL_DestroyTexture(kv.second.tex);
    g_textCache.clear();
    // v2.19: also clear the glyph cache (glyph textures belong to the
    // same renderer and must be released before it goes away).
    clearGlyphCache();
}

static SDL_Texture* cachedTextureFor(SDL_Renderer* ren,
                                     const std::string& url,
                                     SDL_Surface* surface) {
    if (!surface) return nullptr;
    auto it = g_textureCache.find(url);
    if (it != g_textureCache.end()) return it->second;
    SDL_Texture* tex = SDL_CreateTextureFromSurface(ren, surface);
    if (tex) g_textureCache[url] = tex;
    return tex;
}

SDL_Texture* sharedTextureFor(SDL_Renderer* ren, const std::string& key,
                              SDL_Surface* surface) {
    return cachedTextureFor(ren, key, surface);
}

void clearImageTextureCache() {
    for (auto& [k, t] : g_textureCache) {
        (void)k;
        if (t) SDL_DestroyTexture(t);
    }
    g_textureCache.clear();
    clearMediaTextures();
}

// ---------------------------------------------------------------------------
// Internal media widgets (<video> / <audio>)
//
// The media engine (media/mediaplayer.*) decodes on worker threads and
// exposes the latest RGBA frame. This side keeps one STREAMING texture
// per player URL and re-uploads it only when the frame's sequence number
// moved — a paused video costs zero texture uploads, a playing one costs
// exactly one SDL_UpdateTexture per decoded frame.
// ---------------------------------------------------------------------------
struct MediaTexEntry {
    SDL_Texture* tex = nullptr;
    uint64_t seq = 0;
    int w = 0, h = 0;
};
static std::unordered_map<std::string, MediaTexEntry> g_mediaTex;

void clearMediaTextures() {
    for (auto& kv : g_mediaTex)
        if (kv.second.tex) SDL_DestroyTexture(kv.second.tex);
    g_mediaTex.clear();
}

static SDL_Texture* mediaTextureFor(SDL_Renderer* ren, const std::string& url,
                                    media::MediaPlayer* p) {
    if (!p) return nullptr;
    media::MediaFrame f;
    if (!p->copyFrame(f) || f.w <= 0 || f.h <= 0 || f.rgba.empty())
        return nullptr;
    MediaTexEntry& e = g_mediaTex[url];
    if (e.tex && e.seq == f.seq && e.w == f.w && e.h == f.h) return e.tex;
    if (!e.tex || e.w != f.w || e.h != f.h) {
        if (e.tex) SDL_DestroyTexture(e.tex);
        e.tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888,
                                  SDL_TEXTUREACCESS_STREAMING, f.w, f.h);
        e.w = f.w;
        e.h = f.h;
        e.seq = 0;
        if (!e.tex) return nullptr;
    }
    // AV_PIX_FMT_RGBA is byte order R,G,B,A — exactly SDL's little-endian
    // ABGR8888 layout, so the upload is a straight memcpy.
    SDL_UpdateTexture(e.tex, nullptr, f.rgba.data(), f.w * 4);
    e.seq = f.seq;
    return e.tex;
}

// Control-bar geometry, in BOX-LOCAL coordinates (origin = box top-left).
// mediaHitTest() and drawMediaControls() both derive from this, so a
// click lands exactly on what was drawn.
static constexpr int kMediaBarH = 30;
struct MediaGeom {
    SDL_Rect bar, play, seek, mute, vol;
    int timeX = 0, timeW = 0;
    bool hasControls = false;
};

static MediaGeom mediaGeom(const Box& b, bool hasControls) {
    MediaGeom g{};
    g.hasControls = hasControls;
    int w = b.w, h = b.h;
    if (b.isAudio) g.bar = {0, 0, w, h};
    else           g.bar = {0, h - std::min(kMediaBarH, h), w,
                            std::min(kMediaBarH, h)};
    if (!hasControls) return g;
    int by = g.bar.y, bh = g.bar.h;
    int bs = std::min(22, bh - 6);              // square button size
    g.play = {4, by + (bh - bs) / 2, bs, bs};
    int x = g.play.x + bs + 8;
    int right = w - 6;
    int volW = std::min(34, std::max(18, w / 12));
    int timeW = 64;
    g.vol = {right - volW, by + (bh - 5) / 2, volW, 5};
    right -= volW + 6;
    g.mute = {right - bs, by + (bh - bs) / 2, bs, bs};
    right -= bs + 6;
    g.timeW = timeW;
    g.timeX = right - timeW;
    g.seek = {x, by + (bh - 6) / 2, std::max(24, g.timeX - 8 - x), 6};
    return g;
}

// "1:23 / 4:05" for the status readout.
static std::string mediaTimeStr(double s) {
    if (s < 0) s = 0;
    int total = (int)s;
    int h = total / 3600, m = (total % 3600) / 60, sec = total % 60;
    char buf[32];
    if (h > 0) snprintf(buf, sizeof buf, "%d:%02d:%02d", h, m, sec);
    else       snprintf(buf, sizeof buf, "%d:%02d", m, sec);
    return buf;
}

static bool mediaBoxHasControls(const Box& b) {
    auto n = b.sourceNode.lock();
    return n && n->attrs.count("controls");
}

MediaHit mediaHitTest(const Box& b, int docX, int docY) {
    MediaHit hit;
    int lx = docX - b.x, ly = docY - b.y;
    if (lx < 0 || ly < 0 || lx >= b.w || ly >= b.h) return hit;
    bool hasControls = mediaBoxHasControls(b);
    if (!hasControls) { hit.part = MediaHit::Body; return hit; }
    MediaGeom g = mediaGeom(b, true);
    auto in = [](const SDL_Rect& r, int x, int y) {
        return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
    };
    if (in(g.play, lx, ly)) { hit.part = MediaHit::Play; return hit; }
    if (in(g.mute, lx, ly)) { hit.part = MediaHit::Mute; return hit; }
    if (in(g.vol, lx, ly)) {
        hit.part = MediaHit::Volume;
        hit.frac = g.vol.w > 0 ? (double)(lx - g.vol.x) / g.vol.w : 0;
        return hit;
    }
    // Seek track: a few px of vertical slop so thin tracks are grabbable.
    if (g.seek.w > 0 && lx >= g.seek.x - 2 && lx < g.seek.x + g.seek.w + 2 &&
        ly >= g.bar.y - 2 && ly < g.bar.y + g.bar.h + 2) {
        hit.part = MediaHit::Seek;
        hit.frac = std::clamp((double)(lx - g.seek.x) / g.seek.w, 0.0, 1.0);
        return hit;
    }
    // Inside the bar but on no control: nothing (Chrome behaves the same).
    if (ly >= g.bar.y && ly < g.bar.y + g.bar.h) return hit;
    hit.part = MediaHit::Body;
    return hit;
}

static void drawMediaControls(SDL_Renderer* ren, TTF_Font* font,
                              const Box& b, int py,
                              media::MediaPlayer* p) {
    MediaGeom g = mediaGeom(b, true);
    int bx = b.x;

    // Translucent bar background (Chrome-style scrim).
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(ren, 16, 16, 16, 200);
    SDL_Rect bar = {bx + g.bar.x, py + g.bar.y, g.bar.w, g.bar.h};
    SDL_RenderFillRect(ren, &bar);
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_NONE);

    // Play / pause glyph.
    bool playing = p && p->playing();
    SDL_SetRenderDrawColor(ren, 240, 240, 240, 255);
    SDL_Rect pr = {bx + g.play.x, py + g.play.y, g.play.w, g.play.h};
    if (playing) {
        SDL_Rect b1 = {pr.x + 4, pr.y + 3, 4, pr.h - 6};
        SDL_Rect b2 = {pr.x + pr.w - 8, pr.y + 3, 4, pr.h - 6};
        SDL_RenderFillRect(ren, &b1);
        SDL_RenderFillRect(ren, &b2);
    } else {
        int cx = pr.x + pr.w / 2 - 1, cy = pr.y + pr.h / 2;
        for (int dy = -5; dy <= 5; ++dy) {
            int len = 6 - std::abs(dy);
            SDL_RenderDrawLine(ren, cx - 3, cy + dy,
                               cx - 3 + len, cy + dy);
        }
    }

    // Seek track + progress + knob.
    double dur = p ? p->duration() : 0.0;
    double pos = p ? p->position() : 0.0;
    double frac = dur > 0 ? std::clamp(pos / dur, 0.0, 1.0) : 0.0;
    SDL_Rect tr = {bx + g.seek.x, py + g.seek.y, g.seek.w, g.seek.h};
    SDL_SetRenderDrawColor(ren, 95, 95, 95, 255);
    SDL_RenderFillRect(ren, &tr);
    if (g.seek.w > 2) {
        SDL_SetRenderDrawColor(ren, 232, 62, 54, 255);   // chrome red
        SDL_Rect done = {tr.x, tr.y,
                         (int)(tr.w * frac), tr.h};
        if (done.w > 0) SDL_RenderFillRect(ren, &done);
        int kx = tr.x + (int)(tr.w * frac);
        SDL_SetRenderDrawColor(ren, 245, 245, 245, 255);
        SDL_Rect knob = {kx - 3, tr.y - 2, 7, tr.h + 4};
        SDL_RenderFillRect(ren, &knob);
    }

    // Time readout.
    if (font) {
        std::string txt = dur > 0
            ? mediaTimeStr(pos) + " / " + mediaTimeStr(dur)
            : mediaTimeStr(pos);
        TTF_Font* small = getFontForFamily("sans-serif", 11, false, false);
        if (!small) small = font;
        int tw = 0, th = 0;
        SDL_Texture* t = cachedTextTexture(ren, small, txt,
                                           {230, 230, 230, 255}, &tw, &th);
        if (t) {
            SDL_Rect dst = {bx + g.timeX,
                            py + g.bar.y + (g.bar.h - th) / 2, tw, th};
            SDL_RenderCopy(ren, t, nullptr, &dst);
        }
    }

    // Mute toggle: speaker + (crossed out when muted).
    SDL_SetRenderDrawColor(ren, 240, 240, 240, 255);
    SDL_Rect mr = {bx + g.mute.x, py + g.mute.y, g.mute.w, g.mute.h};
    int mcx = mr.x + mr.w / 2, mcy = mr.y + mr.h / 2;
    SDL_Rect spk = {mcx - 6, mcy - 3, 4, 6};
    SDL_RenderFillRect(ren, &spk);
    for (int dy = -3; dy <= 3; ++dy) {
        int len = 4 - std::abs(dy);
        SDL_RenderDrawLine(ren, mcx - 2, mcy + dy, mcx - 2 + len + 1, mcy + dy);
    }
    if (p && p->muted()) {
        SDL_RenderDrawLine(ren, mr.x + 2, mr.y + 2, mr.x + mr.w - 3, mr.y + mr.h - 3);
        SDL_RenderDrawLine(ren, mr.x + mr.w - 3, mr.y + 2, mr.x + 2, mr.y + mr.h - 3);
    }

    // Volume track.
    SDL_SetRenderDrawColor(ren, 95, 95, 95, 255);
    SDL_Rect vr = {bx + g.vol.x, py + g.vol.y, g.vol.w, g.vol.h};
    SDL_RenderFillRect(ren, &vr);
    if (!p || !p->muted()) {
        SDL_SetRenderDrawColor(ren, 240, 240, 240, 255);
        SDL_Rect vf = {vr.x, vr.y, (int)(vr.w * (p ? p->volume() : 0.9f)), vr.h};
        if (vf.w > 0) SDL_RenderFillRect(ren, &vf);
    }
}

static void drawMediaBox(SDL_Renderer* ren, TTF_Font* font,
                         const Box& b, int scrollY) {
    int px = b.x, py = b.y - scrollY;
    bool hasControls = mediaBoxHasControls(b);
    auto p = b.mediaPath.empty() ? nullptr : media::findPlayer(b.mediaPath);

    // Stage: black for video, light grey chrome-style bar for audio.
    SDL_SetRenderDrawColor(ren, b.isAudio ? 236 : 10,
                                b.isAudio ? 236 : 10,
                                b.isAudio ? 238 : 12, 255);
    SDL_Rect box = {px, py, b.w, b.h};
    SDL_RenderFillRect(ren, &box);

    if (b.isVideo) {
        SDL_Texture* tex = mediaTextureFor(ren, b.mediaPath, p.get());
        if (!tex && !b.posterPath.empty()) {
            CachedImage* poster = getImage(b.posterPath);
            if (poster && poster->surface)
                tex = cachedTextureFor(ren, "poster:" + b.posterPath,
                                       poster->surface);
        }
        if (tex) {
            SDL_Rect dst = {px, py, b.w, b.h};
            SDL_RenderCopy(ren, tex, nullptr, &dst);
        } else {
            // No frame yet: status line centered in the dark stage.
            std::string msg = p ? (p->failed() ? p->error() : "Loading\xE2\x80\xA6")
                                : (b.mediaPath.empty() ? "No source"
                                                       : "Loading\xE2\x80\xA6");
            if (msg.size() > 60) msg = msg.substr(0, 60) + "\xE2\x80\xA6";
            TTF_Font* small = getFontForFamily("sans-serif", 12, false, false);
            if (!small) small = font;
            int tw = 0, th = 0;
            SDL_Texture* t = cachedTextTexture(ren, small, msg,
                                               {170, 170, 170, 255}, &tw, &th);
            if (t) {
                SDL_Rect dst = {px + std::max(4, (b.w - tw) / 2),
                                py + std::max(2, (b.h - th) / 2), tw, th};
                SDL_RenderCopy(ren, t, nullptr, &dst);
            }
        }
    } else {
        // Audio element: title-ish label on the left of the bar zone.
        TTF_Font* small = getFontForFamily("sans-serif", 11, false, false);
        if (small && !hasControls) {
            int tw = 0, th = 0;
            SDL_Texture* t = cachedTextTexture(ren, small, "audio",
                                               {90, 90, 90, 255}, &tw, &th);
            if (t) {
                SDL_Rect dst = {px + 8, py + (b.h - th) / 2, tw, th};
                SDL_RenderCopy(ren, t, nullptr, &dst);
            }
        }
    }

    // Hairline outline so the widget reads as an interactive surface.
    SDL_SetRenderDrawColor(ren, 70, 70, 70, 255);
    SDL_RenderDrawRect(ren, &box);

    if (hasControls) drawMediaControls(ren, font, b, py, p.get());
}


// Fetch-and-decode helper for CSS background images. Returns a cached
// surface (local file via ResourceLoader, remote via libcurl) or nullptr.
static CachedImage* bgImageFor(const std::string& pathOrUrl) {
    if (pathOrUrl.empty()) return nullptr;
    return getImage(pathOrUrl);
}

static void drawUnderline(SDL_Renderer* ren, int x, int y, int w, int h, SDL_Color c) {
    SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, 255);
    SDL_Rect u = {x, y + h - 2, w, 1};
    SDL_RenderFillRect(ren, &u);
}

// ---------------------------------------------------------------------------
// Viewport culling
//
// A tall page (Wikipedia article, YouTube results) lays out 2000-5000
// boxes; the viewport shows a few dozen. Everything off-screen used to be
// "rendered" anyway — every hash lookup, every SDL_RenderCopy of every
// text run and image — which made scrolling and typing in the address bar
// burn CPU on work with zero visual result. Boxes (and individual lines
// inside tall boxes) outside the viewport with a small margin are now
// skipped outright.
// ---------------------------------------------------------------------------
static constexpr int kCullMargin = 64;

static void outputSize(SDL_Renderer* ren, int& vw, int& vh) {
    if (SDL_GetRendererOutputSize(ren, &vw, &vh) != 0) {
        vw = 1920; vh = 1080;   // sane fallback; output size is known always
    }
}

static bool boxVisible(const Box& b, int scrollY, int vw, int vh) {
    int top = b.y - scrollY;
    if (top + b.h <= -kCullMargin || top > vh + kCullMargin) return false;
    if (b.x + b.w <= -kCullMargin || b.x > vw + kCullMargin) return false;
    return true;
}

static void renderTextAndImages(SDL_Renderer* ren, TTF_Font* font,
                                const std::vector<Box>& boxes, int scrollY,
                                const std::string& hoveredHref,
                                const std::vector<std::pair<int,int>>& highlightLines,
                                int currentHighlight,
                                const std::shared_ptr<Node>& focusedNode,
                                int focusedCaret,
                                const std::vector<SelectionSpan>* selection);

static void drawBorder(SDL_Renderer* ren, const Box& b, int scrollY) {
    const Edges& bd = b.style.border;
    if (bd.vertical() == 0 && bd.horizontal() == 0) return;
    SDL_Rect box = {b.x, b.y - scrollY, b.w, b.h};
    int t = bd.top, rgt = bd.right, btm = bd.bottom, lft = bd.left;
    // "border: 1px solid transparent" must draw nothing — skip sides
    // whose color carries a 0 alpha (parseColor maps transparent to it).
    if (t > 0 && b.style.borderTopColor.a != 0) {
        SDL_SetRenderDrawColor(ren, b.style.borderTopColor.r,
                               b.style.borderTopColor.g,
                               b.style.borderTopColor.b, 255);
        SDL_Rect top = {box.x, box.y, box.w, t};
        SDL_RenderFillRect(ren, &top);
    }
    if (btm > 0 && b.style.borderBottomColor.a != 0) {
        SDL_SetRenderDrawColor(ren, b.style.borderBottomColor.r,
                               b.style.borderBottomColor.g,
                               b.style.borderBottomColor.b, 255);
        SDL_Rect bot = {box.x, box.y + box.h - btm, box.w, btm};
        SDL_RenderFillRect(ren, &bot);
    }
    if (lft > 0 && b.style.borderLeftColor.a != 0) {
        SDL_SetRenderDrawColor(ren, b.style.borderLeftColor.r,
                               b.style.borderLeftColor.g,
                               b.style.borderLeftColor.b, 255);
        SDL_Rect left = {box.x, box.y + t, lft, box.h - t - btm};
        SDL_RenderFillRect(ren, &left);
    }
    if (rgt > 0 && b.style.borderRightColor.a != 0) {
        SDL_SetRenderDrawColor(ren, b.style.borderRightColor.r,
                               b.style.borderRightColor.g,
                               b.style.borderRightColor.b, 255);
        SDL_Rect right = {box.x + box.w - rgt, box.y + t, rgt, box.h - t - btm};
        SDL_RenderFillRect(ren, &right);
    }
}

// NOTE: render()/renderWithFocus() are CONTENT passes only — they do not
// clear or present. The frame owner (Browser::paint) clears the backbuffer,
// draws the chrome, and calls SDL_RenderPresent once per frame. The old
// code cleared + presented inside these passes, which pushed a half-drawn
// frame (content without chrome) to the screen every frame under a real
// window manager — the address bar was wiped before it ever showed.
void render(SDL_Renderer* ren, TTF_Font* font,
            const std::vector<Box>& boxes, int scrollY,
            const std::string& hoveredHref) {
    (void)hoveredHref;  // hover coloring happens in renderTextAndImages
    int vw = 0, vh = 0;
    outputSize(ren, vw, vh);

    // Pass 1: backgrounds
    for (auto& b : boxes) {
        if (!boxVisible(b, scrollY, vw, vh)) continue;
        if (b.style.visibilityHidden || b.style.opacity <= 0.001f) continue;
        bool hasCustomBg = (b.style.bg.r != 255 ||
                            b.style.bg.g != 255 ||
                            b.style.bg.b != 255) &&
                           b.style.bg.a != 0;   // transparent = no fill
        if (hasCustomBg) {
            SDL_SetRenderDrawColor(ren, b.style.bg.r, b.style.bg.g, b.style.bg.b,
                                   b.style.bg.a);
            SDL_Rect bg = {b.x, b.y - scrollY, b.w, b.h};
            SDL_RenderFillRect(ren, &bg);
        }
    }

    // Pass 1.5: borders. Draw borders on ALL non-image boxes that have
    // a non-zero border width, including background-only boxes (e.g.
    // <div style="background: red; border: 2px solid black"></div>) that
    // have no text and therefore empty `lines`. The old condition
    // `!b.lines.empty()` skipped them, so the border was silently dropped.
    for (auto& b : boxes) {
        if (!boxVisible(b, scrollY, vw, vh)) continue;
        if (b.isImage) continue;
        const Edges& bd = b.style.border;
        if (bd.vertical() == 0 && bd.horizontal() == 0) continue;
        drawBorder(ren, b, scrollY);
    }

    renderTextAndImages(ren, font, boxes, scrollY, hoveredHref,
                        /*highlightLines=*/{}, /*currentHighlight=*/-1,
                        /*focusedNode=*/nullptr, /*focusedCaret=*/-1,
                        /*selection=*/nullptr);
}

void renderWithHighlights(SDL_Renderer* ren, TTF_Font* font,
                          const std::vector<Box>& boxes, int scrollY,
                          const std::string& hoveredHref,
                          const std::vector<std::pair<int,int>>& highlightLines,
                          int currentHighlight) {
    renderWithFocus(ren, font, boxes, scrollY, hoveredHref,
                    highlightLines, currentHighlight, nullptr);
}

void renderWithFocus(SDL_Renderer* ren, TTF_Font* font,
                     const std::vector<Box>& boxes, int scrollY,
                     const std::string& hoveredHref,
                     const std::vector<std::pair<int,int>>& highlightLines,
                     int currentHighlight,
                     const std::shared_ptr<Node>& focusedNode,
                     int focusedCaret,
                     const std::vector<SelectionSpan>* selection) {
    // No clear/present here (see note above render()): this is a
    // mid-frame content pass. The caller owns the backbuffer lifecycle.
    int vw = 0, vh = 0;
    outputSize(ren, vw, vh);

    for (auto& b : boxes) {
        if (!boxVisible(b, scrollY, vw, vh)) continue;
        if (b.style.visibilityHidden || b.style.opacity <= 0.001f) continue;
        bool hasCustomBg = (b.style.bg.r != 255 ||
                            b.style.bg.g != 255 ||
                            b.style.bg.b != 255) &&
                           b.style.bg.a != 0;   // transparent = no fill
        if (hasCustomBg) {
            SDL_SetRenderDrawColor(ren, b.style.bg.r, b.style.bg.g, b.style.bg.b,
                                   b.style.bg.a);
            SDL_Rect bg = {b.x, b.y - scrollY, b.w, b.h};
            SDL_RenderFillRect(ren, &bg);
        }
        // CSS background-image (stretch to the box).
        if (!b.bgImagePath.empty()) {
            CachedImage* img = bgImageFor(b.bgImagePath);
            if (img && img->surface) {
                SDL_Texture* tex = cachedTextureFor(ren, "bg:" + b.bgImagePath,
                                                    img->surface);
                if (tex) {
                    SDL_Rect dst = {b.x, b.y - scrollY, b.w, b.h};
                    SDL_RenderCopy(ren, tex, nullptr, &dst);
                }
            }
        }
    }
    // Pass 1.5: borders. If a box's sourceNode is the focused node,
    // draw a 2px blue focus ring around it on top of the regular border.
    for (auto& b : boxes) {
        if (!boxVisible(b, scrollY, vw, vh)) continue;
        if (b.isImage) continue;
        const Edges& bd = b.style.border;
        if (bd.vertical() == 0 && bd.horizontal() == 0) continue;
        drawBorder(ren, b, scrollY);
    }
    if (focusedNode) {
        for (auto& b : boxes) {
            auto sn = b.sourceNode.lock();
            if (sn && sn == focusedNode) {
                SDL_SetRenderDrawColor(ren, 0x1a, 0x4f, 0xa0, 255);
                SDL_Rect fr = {b.x - 2, b.y - scrollY - 2, b.w + 4, b.h + 4};
                SDL_RenderDrawRect(ren, &fr);
                SDL_Rect fr2 = {b.x - 1, b.y - scrollY - 1, b.w + 2, b.h + 2};
                SDL_RenderDrawRect(ren, &fr2);
                break;
            }
        }
    }

    renderTextAndImages(ren, font, boxes, scrollY, hoveredHref,
                        highlightLines, currentHighlight, focusedNode,
                        focusedCaret, selection);
}

// Shared pass 2: text and images. Optionally draws a yellow highlight
// behind the listed lines (used by find-in-page). `currentHighlight`
// is the index into `highlightLines` of the "current" match (drawn in
// orange) or -1 if none. If `focusedNode` is non-null and matches a
// box's sourceNode, a blinking caret is drawn at the caret position.
static void renderTextAndImages(SDL_Renderer* ren, TTF_Font* font,
                                const std::vector<Box>& boxes, int scrollY,
                                const std::string& hoveredHref,
                                const std::vector<std::pair<int,int>>& highlightLines,
                                int currentHighlight,
                                const std::shared_ptr<Node>& focusedNode,
                                int focusedCaret,
                                const std::vector<SelectionSpan>* selection) {

    const SDL_Color hoverColor = {220, 60, 60, 255};
    const SDL_Color hlYellow   = {255, 235, 130, 255};
    const SDL_Color hlOrange   = {255, 175, 60,  255};
    const SDL_Color selBlue    = {51, 118, 245, 110};   // translucent

    // Selection lookup for a (box, line); returns nullptr when none.
    auto selectionFor = [&](int bi, int li) -> const SelectionSpan* {
        if (!selection) return nullptr;
        for (auto& s : *selection)
            if (s.boxIdx == bi && s.lineIdx == li) return &s;
        return nullptr;
    };

    int vw = 0, vh = 0;
    outputSize(ren, vw, vh);

    // Build a quick lookup: (boxIdx, lineIdx) -> isCurrent, isMatch
    // so we can draw a highlight rect before the text of that line.
    auto isHighlighted = [&](int bi, int li, bool& current) {
        current = false;
        for (size_t k = 0; k < highlightLines.size(); ++k) {
            if (highlightLines[k].first == bi && highlightLines[k].second == li) {
                if ((int)k == currentHighlight) current = true;
                return true;
            }
        }
        return false;
    };

    for (size_t bi = 0; bi < boxes.size(); ++bi) {
        const Box& b = boxes[bi];
        if (!boxVisible(b, scrollY, vw, vh)) continue;
        if (b.style.visibilityHidden || b.style.opacity <= 0.001f) continue;
        if (b.isVideo || b.isAudio) {
            drawMediaBox(ren, font, b, scrollY);
            continue;
        }
        if (b.isImage) {
            int px = b.x;
            int py = b.y - scrollY;
            CachedImage* img = b.imagePath.empty() ? nullptr : getImage(b.imagePath);
            if (img && img->surface) {
                SDL_Texture* tex = cachedTextureFor(ren, b.imagePath, img->surface);
                if (tex) {
                    SDL_Rect dst = {px, py, b.drawW, b.drawH};
                    SDL_RenderCopy(ren, tex, nullptr, &dst);
                }
            } else {
                SDL_SetRenderDrawColor(ren, 230, 230, 230, 255);
                SDL_Rect bg = {px, py, b.drawW, b.drawH};
                SDL_RenderFillRect(ren, &bg);
                SDL_SetRenderDrawColor(ren, 180, 180, 180, 255);
                SDL_RenderDrawRect(ren, &bg);
                if (!b.altText.empty() && font) {
                    int aw = 0, ah = 0;
                    SDL_Texture* t = cachedTextTexture(
                        ren, font, b.altText, {80, 80, 80, 255}, &aw, &ah);
                    if (t) {
                        SDL_Rect dst = {px + 4, py + 4, aw, ah};
                        SDL_RenderCopy(ren, t, nullptr, &dst);
                    }
                }
            }
            continue;
        }

        if (b.lines.empty()) continue;
        // overflow:hidden — clip this box's painting (text, inline images)
        // to its own border box. Truncated headlines, sr-only leftovers,
        // carousel slides all rely on it.
        if (b.style.overflowHidden) {
            SDL_Rect clip = {b.x, b.y - scrollY, b.w, b.h};
            if (getenv("MB_CLIPDBG"))
                std::fprintf(stderr, "[rclip] box y=%d clip={%d,%d,%d,%d}\n",
                             b.y, clip.x, clip.y, clip.w, clip.h);
            SDL_RenderSetClipRect(ren, &clip);
        }
        if (getenv("MB_FONTDBG"))
            std::fprintf(stderr, "[paint] box %zu y=%d lines=%zu"
                                 " firstLineRuns=%zu vis=%d op=%.2f\n",
                         bi, b.y, b.lines.size(),
                         b.lines[0].size(),
                         (int)b.style.visibilityHidden, b.style.opacity);

        int textOffsetX = b.style.padding.left + b.style.border.left;
        int textOffsetY = b.style.padding.top  + b.style.border.top;
        int lineY = b.y + 2 + textOffsetY - scrollY;
        // Translucent selection rects need alpha blending on; restore
        // the default (opaque) mode when the pass is done.
        bool blendPushed = false;
        if (selection) {
            SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
            blendPushed = true;
        }
        // Effective text-align for this box: start/end resolve against
        // the box's base direction; no declaration defaults to start.
        std::string ta = b.style.textAlign;
        if (!b.style.hasTextAlign || ta == "start")
            ta = b.rtl ? "right" : "left";
        else if (ta == "end")
            ta = b.rtl ? "left" : "right";
        for (size_t li = 0; li < b.lines.size(); ++li) {
            const auto& line = b.lines[li];
            if (getenv("MB_FONTDBG"))
                std::fprintf(stderr, "[line] box=%zu li=%zu runs=%zu\n",
                             bi, li, line.size());
            int curLineH = (li < b.lineHeights.size())
                         ? b.lineHeights[li]
                         : b.style.fontSize + 6;

            // Line-level culling: lineY only ever grows, so lines above
            // the viewport are skipped and the loop breaks outright at
            // the first line below it. A <pre> with 3000 lines or the
            // body box wrapping a whole article cost the same per frame
            // as a headline before this.
            if (lineY + curLineH < -kCullMargin) { lineY += curLineH; continue; }
            if (lineY > vh + kCullMargin) break;

            // Find highlight (if any) for this line.
            bool cur = false;
            if (isHighlighted((int)bi, (int)li, cur)) {
                SDL_Color hc = cur ? hlOrange : hlYellow;
                SDL_SetRenderDrawColor(ren, hc.r, hc.g, hc.b, 255);
                SDL_Rect hr = {b.x, lineY - 1, b.w, curLineH};
                SDL_RenderFillRect(ren, &hr);
            }

            // Resolve each run's texture ONCE — this both measures the
            // line and warms the cache for the draw pass below. RTL
            // lines store runs in visual order, so drawing runs left to
            // right produces the correct display.
            int lineWidth = 0;
            for (auto& run : line) {
                if (run.isImage) { lineWidth += run.drawW; continue; }
                if (run.text.empty()) continue;
                TTF_Font* rf = getFontForRun(run.text, run.style.fontFamily,
                                             run.style.fontSize, run.style.bold,
                                             run.style.italic, font);
                SDL_Color c = run.style.color;
                int tw = 0, th = 0;
                cachedTextTexture(ren, rf, run.text, c, &tw, &th);
                lineWidth += tw;
            }
            // Per-line float insets: lines flowing beside a floated image
            // start (or end, for right-aligned text) inside the shrunken
            // span so they never paint over the float.
            int lineLeftInset = 0, lineRightInset = 0;
            if (li < b.lineInsets.size()) {
                lineLeftInset  = std::max(0, b.lineInsets[li].first);
                lineRightInset = std::max(0, b.lineInsets[li].second);
            }
            int contentBoxW = std::max(0, b.w - b.style.padding.horizontal()
                                             - b.style.border.horizontal()
                                             - 8 - lineLeftInset
                                             - lineRightInset);

            int x = b.x + 4 + textOffsetX + lineLeftInset;
            // text-align: center / right shifts the starting x. RTL boxes
            // start at the right edge unless left/center is explicit.
            if (ta == "center") {
                x += std::max(0, (contentBoxW - lineWidth) / 2);
            } else if (ta == "right") {
                x += std::max(0, contentBoxW - lineWidth) + lineRightInset;
            }

            // Selection state for this line: byte span + starting draw x.
            // Byte offsets index the concatenated run text, exactly what
            // layout::hitTestText produces.
            const SelectionSpan* span = selectionFor((int)bi, (int)li);
            int runByteStart = 0;

            for (auto& run : line) {
                if (getenv("MB_FONTDBG2"))
                    std::fprintf(stderr, "[run] y=%d isImg=%d drawW=%d"
                                         " textEmpty=%d text='%.30s'\n",
                                 b.y, (int)run.isImage, run.drawW,
                                 (int)run.text.empty(), run.text.c_str());
                // Inline image run: draw the decoded surface (or an alt
                // placeholder box) flush with the bottom of the line box,
                // then advance x. These runs carry no text, which is why
                // they used to silently vanish.
                if (run.isImage) {
                    if (run.drawW <= 0 || run.drawH <= 0) continue;
                    int imgTop = lineY + std::max(0, curLineH - run.drawH);
                    CachedImage* img = run.imagePath.empty()
                                     ? nullptr : getImage(run.imagePath);
                    if (img && img->surface) {
                        SDL_Texture* tex = cachedTextureFor(ren, run.imagePath,
                                                            img->surface);
                        if (tex) {
                            SDL_Rect dst = {x, imgTop, run.drawW, run.drawH};
                            SDL_RenderCopy(ren, tex, nullptr, &dst);
                        }
                    } else {
                        SDL_SetRenderDrawColor(ren, 230, 230, 230, 255);
                        SDL_Rect bg = {x, imgTop, run.drawW, run.drawH};
                        SDL_RenderFillRect(ren, &bg);
                        SDL_SetRenderDrawColor(ren, 180, 180, 180, 255);
                        SDL_RenderDrawRect(ren, &bg);
                        if (!run.altText.empty() && font && run.drawW > 30
                            && run.drawH > 14) {
                            int aw = 0, ah = 0;
                            SDL_Texture* alt = cachedTextTexture(
                                ren, font, run.altText, {80, 80, 80, 255}, &aw, &ah);
                            if (alt) {
                                SDL_Rect ad = {x + 3, imgTop + 3,
                                               std::min(aw, run.drawW - 6),
                                               std::min(ah, run.drawH - 6)};
                                SDL_RenderCopy(ren, alt, nullptr, &ad);
                            }
                        }
                    }
                    x += run.drawW;
                    continue;
                }

                if (run.text.empty()) continue;

                TTF_Font* useFont = getFontForRun(run.text, run.style.fontFamily,
                                                  run.style.fontSize,
                                                  run.style.bold,
                                                  run.style.italic, font);

                SDL_Color drawColor = run.style.color;
                if (run.isLink && !hoveredHref.empty() && run.href == hoveredHref) {
                    drawColor = hoverColor;
                }

                int tw = 0, th = 0;
                // Get the font height for vertical positioning. The glyph
                // atlas doesn't return a height until after drawing, so
                // use TTF_FontHeight for the baseline calculation.
                th = TTF_FontHeight(useFont);
                // v2.19: Use glyph atlas for runs longer than 16 bytes
                // (~8+ ASCII chars). Short runs (button labels, alt text)
                // still use the line-level cachedTextTexture (per-color
                // textures are fine for short text). Long runs (paragraph
                // text, article content) use the glyph atlas —
                // O(unique_chars) instead of O(unique_lines), eliminating
                // the TTF_RenderUTF8_Blended bottleneck on text-heavy
                // pages like Wikipedia.
                bool usedGlyphAtlas = false;
                if (run.text.size() > 16) {
                    int gy = lineY + std::max(0, curLineH - th);
                    drawTextWithGlyphs(ren, useFont, run.text,
                                       drawColor, x, gy, tw, th);
                    if (tw > 0) usedGlyphAtlas = true;
                }
                if (!usedGlyphAtlas) {
                    tw = th = 0;
                    SDL_Texture* tex = cachedTextTexture(ren, useFont, run.text,
                                                         drawColor, &tw, &th);
                    if (!tex) { runByteStart += run.text.size(); continue; }
                    int ty = lineY + std::max(0, curLineH - th);
                    SDL_Rect dst = {x, ty, tw, th};
                    SDL_RenderCopy(ren, tex, nullptr, &dst);
                }
                // Selection rectangle behind the covered byte range of
                // this run (pixel-mapped by byte fraction of the run).
                if (span && span->endByte > runByteStart &&
                    span->startByte < runByteStart + (int)run.text.size()) {
                    int from = std::max(span->startByte, runByteStart)
                               - runByteStart;
                    int to = std::min(span->endByte,
                                      runByteStart + (int)run.text.size())
                             - runByteStart;
                    int L = (int)run.text.size();
                    int sx0 = x + (int)((long long)tw * from / L);
                    int sx1 = x + (int)((long long)tw * to / L);
                    if (sx1 > sx0) {
                        SDL_SetRenderDrawColor(ren, selBlue.r, selBlue.g,
                                               selBlue.b, selBlue.a);
                        SDL_Rect srect = {sx0, lineY, sx1 - sx0,
                                          std::max(th, curLineH - 2)};
                        SDL_RenderFillRect(ren, &srect);
                    }
                }
                // v2.19: text was already drawn by either the glyph atlas
                // (long runs) or cachedTextTexture (short runs) above.
                // No second SDL_RenderCopy needed — the old code had the
                // draw call here, but it's now inside the if/else block.
                if (run.style.underline) {
                    SDL_Rect dst = {x, lineY, tw, th};
                    drawUnderline(ren, dst.x, dst.y, tw, th, drawColor);
                }
                x += tw;
                runByteStart += (int)run.text.size();
            }

            // Caret: if this box's sourceNode is the focused input,
            // draw a blinking caret. When focusedCaret >= 0 the caret
            // sits that many bytes into the line's text; otherwise at
            // the end (matches the old behavior).
            if (focusedNode && b.sourceNode.lock() == focusedNode &&
                li + 1 == b.lines.size()) {
                Uint32 ticks = SDL_GetTicks();
                bool on = (ticks / 500) % 2 == 0;
                if (on) {
                    int caretX = x;
                    if (focusedCaret >= 0) {
                        // Width of the text before the caret. The line
                        // may hold several runs; walk them accumulating
                        // widths until we consume `focusedCaret` bytes.
                        int remain = focusedCaret;
                        int startX = b.x + 4 + textOffsetX;
                        if (b.style.hasTextAlign && b.style.textAlign == "center")
                            startX += std::max(0, (contentBoxW - lineWidth) / 2);
                        else if (b.style.hasTextAlign && b.style.textAlign == "right")
                            startX += std::max(0, contentBoxW - lineWidth);
                        caretX = startX;
                        for (auto& r : line) {
                            if (r.text.empty()) continue;
                            TTF_Font* rf2 = font;
                            if (r.style.fontFamily != "sans-serif"
                                || r.style.fontSize != 16
                                || r.style.italic || r.style.bold) {
                                TTF_Font* sized = getFontForFamily(
                                    r.style.fontFamily, r.style.fontSize,
                                    r.style.bold, r.style.italic);
                                if (sized) rf2 = sized;
                            }
                            int take = (int)r.text.size();
                            if (remain >= 0 && remain < take) take = std::max(0, remain);
                            int w2 = 0, h2 = 0;
                            if (take > 0)
                                TTF_SizeUTF8(rf2, r.text.substr(0, take).c_str(), &w2, &h2);
                            caretX += w2;
                            if (remain < take) break;
                            remain -= take;
                        }
                    }
                    int caretY = lineY;
                    int caretH = b.style.fontSize + 4;
                    SDL_SetRenderDrawColor(ren, 0x1a, 0x4f, 0xa0, 255);
                    SDL_Rect caret = {caretX, caretY - 2, 1, caretH};
                    SDL_RenderFillRect(ren, &caret);
                }
            }

            lineY += curLineH;
        }
        if (b.style.overflowHidden) SDL_RenderSetClipRect(ren, nullptr);
        if (blendPushed) SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_NONE);
    }
}

} // namespace browser