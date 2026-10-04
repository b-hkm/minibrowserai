#pragma once
#include "../html/parser.h"
#include "../css/style.h"
#include "../js/jsengine.h"
#include "../layout/layout.h"
#include "../render/renderer.h"
#include "../net/fetch.h"
#include "../media/mediaplayer.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace browser {

// One navigation history entry. `scrollY` remembers where the user was
// on the page so back/forward restore the reading position (browsers
// do this; the old code always reset to the top).
struct HistoryEntry {
    std::string url;    // absolute file path or full http(s) URL
    int scrollY = 0;
};

struct Bookmark {
    std::string url;
    std::string title;
};

// High-level browser application: owns the current page (DOM + CSS + JS +
// layout), the navigation history, and the UI chrome (address bar,
// optional bookmarks bar, status bar, scrollbar). main.cpp just
// constructs one of these, calls handleEvent() per SDL event, and calls
// paint() once per frame.
class Browser {
public:
    Browser(SDL_Window* win, SDL_Renderer* ren, TTF_Font* font);
    ~Browser();

    // Load `target` as the new current page. Accepts:
    //   http(s):// URLs     -> fetched via libcurl
    //   about:home / about:blank
    //   view-source:<target>
    //   local file paths (relative resolved against the current page)
    // Typing a bare domain ("example.com") auto-prepends https://.
    void navigate(const std::string& target);

    // Navigation history. canGoBack/Forward reflect whether the button
    // should be enabled.
    void back();
    void forward();
    void reload();
    bool canGoBack()    const { return historyIndex_ > 0; }
    bool canGoForward() const { return historyIndex_ + 1 < history_.size(); }

    // One SDL event. Returns false when the user has asked to quit.
    bool handleEvent(const SDL_Event& e);

    // Paint the page + chrome. Called once per frame by main.
    void paint();

    // True when the page has been mutated (via JS) and layout needs to
    // be recomputed before the next paint.
    bool needsRelayout() const { return needsRelayout_; }
    void clearRelayoutFlag() { needsRelayout_ = false; }

    // Frame scheduling (see main.cpp's event loop):
    //   tick()          - once per loop iteration: fires JS timers and
    //                     runs the page's deferred scripts after the
    //                     first frame is on screen.
    //   nextWakeupMs()  - how long SDL_WaitEventTimeout may sleep before
    //                     the browser wants a tick (-1 = until an OS
    //                     event arrives; idle costs zero CPU).
    //   needsRepaint()  - the frame on screen is stale.
    void tick();
    int  nextWakeupMs() const;
    bool needsRepaint() const;

    // Navigation fetches run on a worker thread so the UI never freezes
    // while the network is pending (weak device, slow links). The
    // --screenshot tool calls setSynchronousNavigation(true) to get the
    // old blocking behavior: navigate() only returns once the document
    // (and pending images) are in.
    void setSynchronousNavigation(bool s) { syncNav_ = s; }

    int winW() const { return winW_; }
    int winH() const { return winH_; }

    // Heights of the UI chrome (in window pixels).
    static constexpr int ADDRESS_BAR_H  = 32;
    static constexpr int BOOKMARKS_H    = 26;
    static constexpr int STATUS_BAR_H   = 22;
    static constexpr int SCROLLBAR_W    = 12;

    // The content area = the rectangle inside the chrome where page
    // content is drawn. Shifts down while the bookmarks bar is visible.
    int contentX() const { return 0; }
    int contentY() const { return ADDRESS_BAR_H + (bookmarksVisible_ ? BOOKMARKS_H : 0); }
    int contentW() const { return winW_; }
    int contentH() const { return winH_ - contentY() - STATUS_BAR_H; }

private:
    SDL_Window*   win_;
    SDL_Renderer* ren_;
    TTF_Font*     font_;
    int winW_ = 800, winH_ = 600;

    // Current page state
    std::string currentPath_;       // URL or absolute path shown in the bar
    std::string baseDir_;           // file dir (local pages) or page URL (remote)
    std::string documentUrl_;       // final remote URL (after redirects), "" for files
    std::string rawSource_;         // original HTML (view-source / save page)
    std::string pageInfo_;          // status-bar info: "HTTP 200 · text/html"
    std::shared_ptr<Node> dom_;
    std::vector<CSSRule> cssRules_;
    std::vector<CSSRule> baseCssRules_;
    LayoutResult layout_;
    JSEngine js_;
    int  scrollY_ = 0;
    std::string hoveredHref_;
    std::shared_ptr<Node> hoveredNode_;  // for CSS :hover
    bool needsRelayout_ = false;

    // Deferred page scripts + frame scheduling. showPage_ collects the
    // <script> chunks; tick() runs them right after the first frame is
    // presented, so first paint never waits on JS fetch/execution.
    std::vector<JSScriptChunk> pendingJs_;
    bool hasDeferredJs_ = false;
    bool paintedOnce_   = false;
    bool frameDirty_    = true;   // paint as soon as the loop starts

    // Async document fetch (see setSynchronousNavigation). The worker
    // only touches the heap-owned slot below plus its own copies; the
    // slot is shared_ptr-held by BOTH the browser and the worker, so a
    // page still loading at quit time can never write into a destroyed
    // Browser — the result simply lands in a slot nobody reads.
    bool syncNav_ = false;
    struct NavSlot {
        mutable std::mutex m;               // mutable: const wakeup check locks it
        std::unique_ptr<FetchResult> res;   // guarded by m
        std::atomic<int> doneSeq{0};        // generation whose result is in res
    };
    std::atomic<int> navSeq_{0};            // generation of the request in flight
    std::shared_ptr<NavSlot> navSlot_ = std::make_shared<NavSlot>();
    std::string navOrigUrl_;                // pre-shim URL (main thread only)
    void startAsyncFetch_(const std::string& fetchUrlStr,
                          const std::string& originalUrl,
                          const std::string& bridgeReason = "");
    void finishRemoteLoad_(FetchResult fr, const std::string& originalUrl);

    // Image arrivals from the background pool: repaint immediately, and
    // relayout (dimensions changed) debounced — at most once per 300 ms
    // while a gallery is still landing, plus a final pass when it drains.
    bool   imagesPendingRelayout_ = false;
    Uint32 lastImageArrivalMs_ = 0;
    void pollImageArrivals_();

    // mpv playback (fallback only): the INTERNAL player (media/mediaplayer)
    // now plays every direct media file/URL in-page like Chrome. mpv stays
    // for yt-dlp-only targets (YouTube watch pages) and the "v" key / mpv:
    // links as an explicit escape hatch.
    void maybePlayInMpv_(const std::string& url);
    // When a launch is reported OK, tick() re-checks ~2 s later whether
    // mpv is still alive and surfaces its log tail if it died instantly.
    Uint32 mpvCheckDueMs_ = 0;

    // ---- internal media (<video>/<audio>) --------------------------------
    // relayout_() acquires one media::MediaPlayer per media box (registry
    // keyed by resolved URL) and applies loop/muted/autoplay attributes.
    // tick() repaints while any player runs or produces frames.
    std::unordered_set<std::string> autoplayStarted_;
    uint64_t mediaSeenSeq_ = 0;     // last frame counter seen by tick()
    Uint32   lastMediaPaintMs_ = 0; // video repaint throttle (~30 fps cap)
    int      seekDragBox_ = -1;     // box index whose seek bar is being dragged
    void applyAttrsToPlayers_();
    // Chrome-style built-in page for a direct media URL/file (the old code
    // spawned mpv; now the video plays inside the browser window).
    // srcOverride/mediaTitle: yt-dlp bridge form — the page URL stays the
    // original watch link (address bar, history, mpv: hatch) while the
    // <video> src is the extracted progressive stream.
    void loadMediaPage_(const std::string& url,
                        const std::string& srcOverride = "",
                        const std::string& mediaTitle = "",
                        const std::string& via = "");

    // ---- yt-dlp bridge (YouTube watch URLs -> internal player) -----------
    // A watch-style URL is resolved to a progressive stream URL by an
    // external yt-dlp/youtube-dl process and played on the built-in media
    // page — in-process playback, no mpv. The resolve runs off the UI
    // thread (a cold resolve spawns a process + network round-trips);
    // completion is polled by tick() through the inbox below. Results are
    // keyed by generation, so an overlapping stale resolve can never
    // clobber the current one. Failures fall back to the normal page
    // load (v2.8: the YouTube page rendered from ytInitialData — the
    // ytInitialData extractor in app/shims.cpp reads the JSON already
    // embedded in the page and renders a clean static page; v2.7 had
    // removed the extractor entirely, which made YouTube render blank
    // because the SPA needs JS the engine can't run) with the reason in
    // the status bar.
    struct BridgeResult {
        bool ok = false;
        std::string originalUrl, directUrl, title, ext, via, err;
    };
    struct BridgeSlot {
        mutable std::mutex m;
        std::map<int, BridgeResult> inbox;   // generation -> result
    };
    std::shared_ptr<BridgeSlot> bridgeSlot_ = std::make_shared<BridgeSlot>();
    int  bridgeGen_ = 0;          // bumped per openExtractedMedia_ call
    bool bridgePending_ = false;  // a resolve for the current gen is in flight
    // Failure note carried across the fallback page load: finishRemoteLoad_
    // would otherwise overwrite the status line ("HTTP 200 ...") before the
    // user ever reads WHY the internal player did not start.
    std::string bridgeNote_;
    void openExtractedMedia_(const std::string& url);
    void pollBridgeResult_();

    // ---- image viewer overlay ---------------------------------------------
    // Click a plain <img> (not inside a link) to open it in a full-content
    // lightbox: scroll = zoom, drag = pan, +/-/0/arrow keys, Esc/X closes.
    bool        viewerOpen_ = false;
    std::string viewerUrl_;
    float       viewerZoom_ = 1.0f;
    int         viewerPanX_ = 0, viewerPanY_ = 0;
    bool        viewerPanning_ = false;
    int         viewerGrabX_ = 0, viewerGrabY_ = 0;
    SDL_Rect    viewerCloseRect_ = {0, 0, 0, 0};
    void openImageViewer_(const std::string& url);
    void closeImageViewer_();
    void drawImageViewer_();
    bool viewerKey_(int sym, Uint16 mod);

    // Test hook (MB_AUTOCLICK="x,y"): one synthetic click (content
    // coords) once the page has settled, so the click->navigate->mpv
    // chain can be e2e-tested without a real pointer. Inert unless the
    // env var is set.
    int    autoClickX_ = -1, autoClickY_ = -1;
    bool   autoClickDone_ = false;
    Uint32 autoClickDueMs_ = 0;

    // Navigation history with scroll positions.
    std::vector<HistoryEntry> history_;
    size_t historyIndex_ = 0;

    // Cached cursors (allocated once, freed in dtor).
    SDL_Cursor* cursorArrow_ = nullptr;
    SDL_Cursor* cursorHand_  = nullptr;
    bool usingHandCursor_ = false;
    int  mouseX_ = 0, mouseY_ = 0;

    // Address bar input. Keystrokes edit addressInput_ at addressCaret_;
    // Enter navigates, Esc cancels.
    bool   addressEditing_ = false;
    std::string addressInput_;
    int    addressCaret_ = 0;
    int    cursorBlinkTick_ = 0;

    // Scrollbar drag state.
    bool   draggingScrollbar_ = false;
    int    dragThumbOffset_ = 0;

    // Find-in-page. After Ctrl+F, the user types a query and we
    // highlight all matches and scroll to the first one. Esc closes,
    // Enter advances, Shift+Enter goes back.
    bool   finding_ = false;
    std::string findQuery_;
    std::vector<std::pair<int,int>> findMatches_;  // (boxIdx, lineIdx)
    size_t findCurrent_ = 0;

    // Mouse text selection (round 3). Positions are layout-space
    // TextPos (box/line/byte in the line's run text). selecting_ is
    // live while the button is down; a non-empty range paints blue and
    // Ctrl+C copies it. Presses on links/widgets keep the old
    // mousedown-activates behavior — selection only starts on plain
    // text or background.
    TextPos selAnchor_, selFocus_;
    bool   selecting_ = false;
    bool   hasSelection_ = false;
    void startSelection_(int contentX, int contentY);
    void updateSelection_(int contentX, int contentY);
    void finishSelection_();
    void clearSelection_();
    void selectWordAt_(int contentX, int contentY);
    std::vector<SelectionSpan> selectionSpans_() const;
    void copySelection_() const;
    // True when the press lands on something clickable (link rect, or a
    // box whose node is an input/button/select/textarea/a or carries an
    // onclick) — those keep the old mousedown-activates behavior.
    bool isActionableHit_(int contentX, int contentY) const;

    // Page zoom: multiplies all font sizes. 1.0 = 100%.
    float  zoom_ = 1.0f;

    // Form input focus. The currently focused <input>/<textarea> node,
    // or nullptr. carets_ holds the caret (byte offset into the value)
    // per focused node so arrow keys / insertion work mid-text.
    std::shared_ptr<Node> focusedNode_;
    std::unordered_map<Node*, int> carets_;
    int  focusedCaret() const;
    void setFocusedCaret(int i);
    int  focusCaretTick_ = 0;

    // Bookmarks bar.
    std::vector<Bookmark> bookmarks_;
    bool bookmarksVisible_ = false;
    // Rects of bookmark buttons in the bar, for click hit-testing.
    std::vector<SDL_Rect> bookmarkRects_;

    // Alert dialog (window.alert).
    bool        alertVisible_ = false;
    std::string alertText_;
    SDL_Rect    alertOkRect_ = {0, 0, 0, 0};

    // Internal helpers
    void loadLocalPage_(const std::string& absolutePath);
    void loadRemotePage_(const std::string& url,
                         const std::string& bridgeReason = "");
    void loadAboutPage_(const std::string& what);
    void loadViewSource_(const std::string& target);
    // dwm/xmonad-style WMs display ONLY the window title — there is no
    // separate URL display anywhere on screen. Compose "Title — URL" so
    // the address is always visible in the WM bar.
    void updateWindowTitle_();
    void showPage_(const std::string& html, const std::string& urlForDisplay,
                   const std::string& baseUrl);
    // Execute the scripts collected by showPage_ (after first paint).
    void runPendingJs_();
    void loadHistoryEntry_(const HistoryEntry& e);
    void relayout_();
    int  maxScroll() const;
    std::string resolveLink_(const std::string& href) const;
    void handleClick(int sx, int sy);
    void handleClickForNode_(std::shared_ptr<Node> node);
    void scrollToFragment_(const std::string& frag);
    bool submitForm_(std::shared_ptr<Node> form);
    void updateHoverCursor();
    void ensureCursors_();

    // UI drawing helpers
    void drawChrome_();
    void drawAddressBar_();
    void drawBookmarksBar_();
    void drawStatusBar_();
    void drawScrollbar_();
    void drawFindBar_();
    void drawAlert_();
    void drawButton_(int x, int y, int w, int h,
                     const std::string& label, bool enabled, bool hovered);
    bool hitButton_(int x, int y, int w, int h, int mx, int my) const {
        return mx >= x && mx < x + w && my >= y && my < y + h;
    }

    // Address bar geometry helper (used by both draw and click).
    void addressBarFieldRect_(int& x, int& y, int& w, int& h) const;

    // Scrollbar geometry helper.
    void scrollbarThumbRect_(int& x, int& y, int& w, int& h) const;

    // Find-in-page helpers.
    void runFind_();           // recompute findMatches_ from findQuery_
    void findNext_(int dir);   // advance (dir=1) or go back (dir=-1)

    // Apply zoom_ (global zoom scale + relayout).
    void applyZoom_();

    // Cycle focus to the next/previous focusable element (<input>,
    // <textarea>, <a href>, <button>). If reverse=true, go backward.
    void advanceFocus_(bool reverse);

    // Walk the DOM in document order collecting all focusable nodes.
    std::vector<std::shared_ptr<Node>> collectFocusable_() const;

    // Bookmarks persistence.
    void loadBookmarks_();
    void saveBookmarks_() const;
    void toggleBookmark_();
    bool isBookmarked_() const;

    // Save the current page's HTML to disk (Ctrl+S).
    void savePage_();

    // Simple text editing helpers shared by the address bar / find bar /
    // focused form inputs. All caret offsets are BYTE offsets that must
    // land on UTF-8 codepoint boundaries.
    static int utf8Prev(const std::string& s, int i);
    static int utf8Next(const std::string& s, int i);
    void pasteClipboard_(std::string& text, int& caret);

    // Resolve a (possibly relative) href against the current page.
    std::string resolveUrl_(const std::string& url) const;
    // Same, but against an explicit directory/URL — lets resolveLink_
    // honor a <base href> that overrides the page's own location.
    std::string resolveWithBase_(const std::string& url,
                                 const std::string& dir) const;
    // The URL all relative references on the current page resolve
    // against: the <base href> when the page declares one, otherwise the
    // page URL (remote) or its directory (local files).
    std::string effectiveBaseUrl_() const;
    // Effective base after <base href> processing; empty = no override.
    std::string docBaseUrl_;

    // Fire a form's submit (Enter in a field) / button (click).
    void fireSubmit_(std::shared_ptr<Node> form);
};

} // namespace browser
