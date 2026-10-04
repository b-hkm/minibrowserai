#pragma once
#include "../layout/layout.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <string>
#include <vector>
#include <utility>

namespace browser {
namespace media { class MediaPlayer; }   // internal <video>/<audio> engine

// One highlighted range of selected text: box/line identify the line,
// startByte/endByte are offsets into that line's concatenated run text
// (the same coordinates layout's TextPos uses). The renderer computes
// the pixel range per run while painting.
struct SelectionSpan {
    int boxIdx = -1, lineIdx = -1;
    int startByte = 0, endByte = 0;
};

// hoveredHref: when non-empty, any <a href="..."> matching this string
// is rendered with the hover color and a "pointer" cursor was set by caller.
void render(SDL_Renderer* r, TTF_Font* font,
            const std::vector<Box>& boxes, int scrollY,
            const std::string& hoveredHref = "");

// ---------------------------------------------------------------------------
// Internal media widgets (<video> / <audio>)
// ---------------------------------------------------------------------------

// Which part of a media box a document-space point lands on. Shared by
// the control-bar drawing and the browser's click hit-testing so what
// you see is exactly what you can press.
struct MediaHit {
    enum Part { None = 0, Body, Play, Seek, Mute, Volume };
    Part part = None;
    double frac = 0;      // Seek/Volume: 0..1 across the track
};

// Hit-test a point (DOCUMENT coordinates: x as laid out, y including
// scroll) against a media box's control geometry.
MediaHit mediaHitTest(const Box& b, int docX, int docY);

// Shared SDL_Texture cache entry point for the app layer: returns the
// cached texture for (key, surface), creating it on first use. The
// browser's image-viewer overlay uses "viewer:<url>" keys; the renderer
// uses raw image paths and "bg:"/"poster:" prefixes internally. Textures
// are owned by the cache and die with clearImageTextureCache().
SDL_Texture* sharedTextureFor(SDL_Renderer* ren, const std::string& key,
                              SDL_Surface* surface);

// Same as render(), but also draws a yellow highlight behind every line
// listed in `highlightLines`. Each entry is (boxIndex, lineIndex) and
// refers to the same index used by the Browser's findMatches_.
void renderWithHighlights(SDL_Renderer* r, TTF_Font* font,
                          const std::vector<Box>& boxes, int scrollY,
                          const std::string& hoveredHref,
                          const std::vector<std::pair<int,int>>& highlightLines,
                          int currentHighlight);

// Same as renderWithHighlights, but also draws a blinking caret inside
// the focused <input>/<textarea>. `focusedCaret` is the caret index in
// characters within the field's value (-1 = at end). When `selection`
// is provided, each covered run segment gets a translucent blue
// selection rectangle behind the glyphs.
void renderWithFocus(SDL_Renderer* r, TTF_Font* font,
                     const std::vector<Box>& boxes, int scrollY,
                     const std::string& hoveredHref,
                     const std::vector<std::pair<int,int>>& highlightLines,
                     int currentHighlight,
                     const std::shared_ptr<Node>& focusedNode,
                     int focusedCaret = -1,
                     const std::vector<SelectionSpan>* selection = nullptr);

// Free every cached SDL_Texture for images. Call this when the SDL_Renderer
// is about to be destroyed (or has been). The textures are tied to the
// renderer, so they must not outlive it.
void clearImageTextureCache();

// Textured-text cache. Rendering one text run previously cost a
// TTF_RenderUTF8_Blended + SDL_CreateTextureFromSurface + destroy PER
// FRAME PER RUN; the cache turns that into a hash lookup after the first
// frame. Returns the cached texture for (font, color, utf8) and its
// pixel size; `w`/`h` may be null. The texture is owned by the cache —
// do not destroy it.
SDL_Texture* cachedTextTexture(SDL_Renderer* ren, TTF_Font* font,
                               const std::string& utf8, SDL_Color color,
                               int* w = nullptr, int* h = nullptr);

// Free every cached text texture. Must be called before the SDL_Renderer
// is destroyed (same contract as clearImageTextureCache).
void clearTextTextureCache();

} // namespace browser
