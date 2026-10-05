#include "browser.h"
#include "shims.h"
#include "mpv.h"
#include "../layout/font_loader.h"
#include "../layout/resource.h"
#include "../media/extractor.h"
#include "../media/mediaplayer.h"
#include "../net/fetch.h"
#include "../net/url.h"
#include "../render/renderer.h"
#include "../render/window.h"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <atomic>
#include <cstdlib>
#include <fstream>
#include <unistd.h>
#include <iostream>
#include <sstream>

namespace browser {

// The yt-dlp bridge lives at browser::media::extractor; inside this file
// it is used ~a dozen times, so give it a short alias.
namespace extractor = media::extractor;

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

Browser::Browser(SDL_Window* win, SDL_Renderer* ren, TTF_Font* font)
    : win_(win), ren_(ren), font_(font) {
    SDL_GetWindowSize(win, &winW_, &winH_);
    ensureCursors_();
    loadBookmarks_();

    // Test hook: MB_AUTOCLICK="x,y" fires one synthetic click (screen
    // coords) right after the first painted frame — lets automated runs
    // exercise the click -> navigate -> mpv chain without a pointer.
    if (const char* ac = getenv("MB_AUTOCLICK")) {
        int ax = -1, ay = -1;
        if (sscanf(ac, "%d,%d", &ax, &ay) == 2 && ax >= 0 && ay >= 0) {
            autoClickX_ = ax;
            autoClickY_ = ay;
            std::cout << "[auto] MB_AUTOCLICK armed at " << ax << ","
                      << ay << "\n";
        }
    }

    js_.onDomMutated = [this]() { needsRelayout_ = true; };
    js_.onInvalidate = [this]() { needsRelayout_ = true; };
    // JS-driven navigation (location.href = ..., location.assign).
    js_.onNavigate = [this](const std::string& url) { navigate(url); };
    // window.alert -> modal dialog drawn over the page.
    js_.onAlert = [this](const std::string& msg) {
        alertVisible_ = true;
        alertText_ = msg;
    };
    js_.onConfirm = [this](const std::string& msg) {
        alertVisible_ = true;
        alertText_ = "CONFIRM: " + msg;
        // confirm() returns false for now (no buttons in the dialog yet).
        return false;
    };
    // el.focus() moves form focus to the node.
    js_.onFocusNode = [this](std::shared_ptr<Node> n) {
        if (n && (n->tag == "input" || n->tag == "textarea")) {
            focusedNode_ = n;
            SDL_StartTextInput();
            needsRelayout_ = true;
        }
    };
    // Element geometry (round 4): back getBoundingClientRect() and the
    // offset* properties. The lambdas read layout_/font_/scrollY_ at
    // CALL time — every relayout_() and scroll automatically refreshes
    // what page JS sees, with no layout pointers held by the JS layer.
    js_.rectForNode = [this](const std::shared_ptr<Node>& n, DOMRect& out) {
        // font_ is the same face layout() used; when no page is loaded
        // layout_ is empty and rectForNode() reports "no fragments".
        return rectForNode(layout_, font_, n, out);
    };
    js_.scrollY = [this]() { return scrollY_; };
    // JS media API (<video>.play() etc.) keys players exactly like layout.
    js_.resolveMedia = [this](const std::string& u) { return resolveLink_(u); };
}

Browser::~Browser() {
    media::stopAllPlayers();   // join decode threads before SDL goes away
    if (cursorArrow_) SDL_FreeCursor(cursorArrow_);
    if (cursorHand_)  SDL_FreeCursor(cursorHand_);
    saveBookmarks_();
    // Cached textures belong to ren_ — release before it goes away.
    clearTextTextureCache();
    clearImageTextureCache();
}

void Browser::ensureCursors_() {
    if (!cursorArrow_) cursorArrow_ = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW);
    if (!cursorHand_)  cursorHand_  = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_HAND);
    SDL_SetCursor(cursorArrow_);
    usingHandCursor_ = false;
}

// ---------------------------------------------------------------------------
// Small text helpers
// ---------------------------------------------------------------------------

// Move to the previous UTF-8 codepoint boundary at or before i.
int Browser::utf8Prev(const std::string& s, int i) {
    if (i <= 0) return 0;
    --i;
    while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) --i;
    return i;
}

// Move to the next UTF-8 codepoint boundary after i.
int Browser::utf8Next(const std::string& s, int i) {
    if (i >= (int)s.size()) return (int)s.size();
    ++i;
    while (i < (int)s.size() && ((unsigned char)s[i] & 0xC0) == 0x80) ++i;
    return i;
}

int Browser::focusedCaret() const {
    if (!focusedNode_) return -1;
    auto it = carets_.find(focusedNode_.get());
    if (it != carets_.end()) return it->second;
    // Default: caret at end of the current value.
    std::string v;
    if (focusedNode_->tag == "input")
        v = focusedNode_->attrs.count("value") ? focusedNode_->attrs["value"] : "";
    else if (focusedNode_->tag == "textarea") {
        for (auto& c : focusedNode_->children)
            if (c->tag == "text") v += c->text;
    }
    return (int)v.size();
}

void Browser::setFocusedCaret(int i) {
    if (focusedNode_) carets_[focusedNode_.get()] = std::max(0, i);
}

void Browser::pasteClipboard_(std::string& text, int& caret) {
    char* clip = SDL_GetClipboardText();
    if (clip) {
        std::string paste = clip;
        SDL_free(clip);
        if (!paste.empty()) {
            paste.erase(std::remove(paste.begin(), paste.end(), '\r'), paste.end());
            text.insert((size_t)std::min(caret, (int)text.size()), paste);
            caret += (int)paste.size();
        }
    }
}

// ---------------------------------------------------------------------------
// Path / URL resolution
// ---------------------------------------------------------------------------

static bool isAbsolutePath(const std::string& p) {
    if (p.empty()) return false;
    if (p[0] == '/' || p[0] == '\\') return true;
    if (p.size() >= 3 && std::isalpha((unsigned char)p[0]) &&
        p[1] == ':' && (p[2] == '/' || p[2] == '\\')) return true;
    return false;
}

static std::string normalizePath(std::string p) {
    for (auto& c : p) if (c == '\\') c = '/';
    bool abs = !p.empty() && p.front() == '/';
    std::vector<std::string> parts;
    std::istringstream iss(p);
    std::string seg;
    while (std::getline(iss, seg, '/')) {
        if (seg.empty() || seg == ".") continue;
        if (seg == "..") {
            if (!parts.empty() && parts.back() != "..") parts.pop_back();
            else if (!abs) parts.push_back("..");
        } else {
            parts.push_back(seg);
        }
    }
    std::string out = abs ? "/" : "";
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) out += "/";
        out += parts[i];
    }
    return out.empty() ? "." : out;
}

std::string Browser::resolveUrl_(const std::string& url) const {
    return resolveWithBase_(url, baseDir_);
}

// Join `url` onto an explicit base directory (local pages) — used both
// by resolveUrl_ (page dir) and by resolveLink_ when a <base href>
// overrides the location.
std::string Browser::resolveWithBase_(const std::string& url,
                                      const std::string& dir) const {
    if (url.empty() || isRemoteUrl(url) || isAbsolutePath(url)) return url;
    std::string joined;
    if (!dir.empty()) {
        char last = dir.back();
        if (last != '/' && last != '\\') joined = dir + "/" + url;
        else                              joined = dir + url;
    } else {
        joined = url;
    }
    return normalizePath(joined);
}

std::string Browser::effectiveBaseUrl_() const {
    if (!docBaseUrl_.empty()) return docBaseUrl_;
    if (!documentUrl_.empty()) return documentUrl_;
    return baseDir_;
}

// Resolve a link href depending on whether the current page is remote.
std::string Browser::resolveLink_(const std::string& href) const {
    if (href.empty()) return href;
    // Pseudo-schemes we intercept in navigate(): never feed them to the
    // generic URL joiner, which would mangle them into a path of the
    // current page (e.g. on local files).
    if (href.compare(0, 4, "mpv:") == 0) return href;
    // Protocol-relative ("//fonts.googleapis.com/..."): isRemoteUrl()
    // says "remote" and would hand the scheme-less URL straight to curl
    // (which fails). Inherit the current document's scheme — https when
    // unknown — before the early return below can keep it bare.
    if (href.size() >= 2 && href[0] == '/' && href[1] == '/' &&
        (href.size() < 3 || href[2] != '/')) {
        std::string scheme = "https";
        size_t colon = documentUrl_.find("://");
        if (colon != std::string::npos && colon <= 5)
            scheme = documentUrl_.substr(0, colon);
        return scheme + ":" + href;
    }
    if (isRemoteUrl(href)) return href;
    // <base href> (or the page URL when no base is declared) wins for
    // every relative reference on remote pages.
    if (isRemoteUrl(effectiveBaseUrl_()))
        return joinUrl(effectiveBaseUrl_(), href);
    if (href.size() >= 1 && href[0] == '#') return href;
    // Local page. A <base href> that is itself a local directory
    // overrides the page dir; otherwise resolve against it as before.
    std::string dir = (!docBaseUrl_.empty() && !isRemoteUrl(docBaseUrl_))
                          ? docBaseUrl_ : baseDir_;
    return resolveWithBase_(href, dir);
}

// ---------------------------------------------------------------------------
// Reading local files / showing parsed pages
// ---------------------------------------------------------------------------

static std::string readFileToString(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Strip a query/fragment that was meant for a remote URL: local file
// loads must not try to open "page.html?x=1".
static std::string stripFileQueryFrag(const std::string& p) {
    size_t cut = p.find_first_of("?#");
    return (cut == std::string::npos) ? p : p.substr(0, cut);
}

// Parse html, collect CSS (inline + local <link>), JS (inline + local
// <script src>), run it, and lay the page out. Used by every loader.
static double msSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();
}

void Browser::showPage_(const std::string& html, const std::string& urlForDisplay,
                        const std::string& baseUrl) {
    auto t0 = std::chrono::steady_clock::now();
    rawSource_ = html;
    currentPath_ = urlForDisplay;
    baseDir_ = baseUrl;
    documentUrl_ = isRemoteUrl(baseUrl) ? baseUrl : "";

    dom_ = parseHTML(html);
    clearSelection_();   // a new document never inherits old text ranges
    std::cerr << "[perf] parseHTML " << msSince(t0) << "ms\n";

    // <base href> override: a page can re-anchor every relative link,
    // stylesheet, script and image on itself (common on CMS-generated
    // pages served from non-root paths). The first <base href> in the
    // document wins; it is resolved against the page URL when relative.
    // Cleared first so a previous page's base can never leak through an
    // error page or about page that skips this block.
    docBaseUrl_.clear();
    {
        std::string rawBase = findBaseHref(dom_);
        if (!rawBase.empty()) {
            if (isRemoteUrl(rawBase))
                docBaseUrl_ = rawBase;
            else if (isRemoteUrl(baseUrl))
                docBaseUrl_ = joinUrl(baseUrl, rawBase);
            else
                docBaseUrl_ = resolveWithBase_(rawBase, baseDir_);
            std::cerr << "[base] <base href> active: " << docBaseUrl_ << "\n";
        }
    }

    // External CSS: <link rel="stylesheet" href="...">. extractCSS() only
    // pulls inline <style> bodies, so we walk the DOM for <link> tags
    // ourselves. Sheets are collected first (deduped by resolved URL —
    // GitHub ships the same sheet under several links), fetched through a
    // thread pool (29 sequential round-trips were ~4.5 s on github.com),
    // then appended in document order.
    //
    // v2.17: NON-BLOCKING CSS fetch. The old code spawned a thread pool
    // to fetch all stylesheets in parallel, then JOINED the pool —
    // blocking first paint by 100-300ms. The new code:
    //   1. Renders immediately with inline <style> CSS only (already in
    //      styleText from extractCSS above).
    //   2. Spawns a background thread to fetch external CSS.
    //   3. pollCssArrival_() (called from tick()) merges the external
    //      CSS when it arrives, re-parses all CSS, re-layouts, and
    //      triggers a repaint.
    // This means the user sees text content within milliseconds of
    // HTML parsing, without waiting for CSS download. The page then
    // "snaps" into its styled form when CSS arrives (~100-300ms later).
    std::string styleText = extractCSS(dom_);
    {
        std::vector<std::string> sheetUrls;          // document order, deduped
        std::function<void(const std::shared_ptr<Node>&)> walkLink =
            [&](const std::shared_ptr<Node>& n) {
                for (auto& c : n->children) {
                    if (c->tag == "link") {
                        auto rel  = c->attrs.find("rel");
                        auto href = c->attrs.find("href");
                        bool isCss = (rel != c->attrs.end() &&
                                      rel->second.find("stylesheet") != std::string::npos);
                        if (isCss && href != c->attrs.end()) {
                            std::string resolved = resolveLink_(href->second);
                            if (std::find(sheetUrls.begin(), sheetUrls.end(),
                                          resolved) == sheetUrls.end())
                                sheetUrls.push_back(resolved);
                        }
                    } else {
                        walkLink(c);
                    }
                }
            };
        walkLink(dom_);

        if (!sheetUrls.empty()) {
            // v2.17: Spawn a background thread to fetch external CSS.
            // The slot is heap-held so it's safe if the Browser is
            // destroyed while CSS is still fetching.
            cssSlot_ = std::make_shared<CssSlot>();
            auto slot = cssSlot_;
            std::thread([slot, sheetUrls, this]() {
                std::vector<std::string> bodies(sheetUrls.size());
                std::atomic<size_t> next{0};
                auto worker = [&]() {
                    for (;;) {
                        size_t i = next++;
                        if (i >= sheetUrls.size()) break;
                        const std::string& resolved = sheetUrls[i];
                        if (isRemoteUrl(resolved)) {
                            FetchResult fr = fetchUrlCached(resolved, 15);
                            bodies[i] = fr.ok ? fr.body : std::string{};
                        } else {
                            bodies[i] = readFileToString(resolved);
                        }
                    }
                };
                int n = std::min<size_t>(8, std::max<size_t>(1, sheetUrls.size()));
                {
                    std::vector<std::thread> pool;
                    for (int t = 0; t < n; ++t) pool.emplace_back(worker);
                    for (auto& th : pool) th.join();
                }
                // Concatenate the fetched CSS in document order.
                std::string externalCss;
                for (size_t i = 0; i < sheetUrls.size(); ++i) {
                    if (!bodies[i].empty()) {
                        externalCss += "\n/* " + sheetUrls[i] + " */\n" + bodies[i];
                    } else {
                        std::cerr << "[css] could not load " << sheetUrls[i] << "\n";
                    }
                }
                // Release the thread's curl handle (short-lived thread).
                releaseThreadCurl();
                // Deliver the result to the UI thread via the slot.
                if (!slot->cancelled.load()) {
                    std::lock_guard<std::mutex> lk(slot->m);
                    slot->externalCss = std::move(externalCss);
                    slot->done.store(true);
                }
            }).detach();
        }
    }
    // Kick off the image cache in the background pool so the layout pass
    // below shows placeholders for anything not yet arrived and the page
    // paints IMMEDIATELY — first paint no longer waits for the whole
    // gallery (tick() repaints + relayouts debounced as images land).
    // Uses the same source-picking logic layout will use (src / data-src /
    // srcset / <picture><source>). Must run BEFORE applyZoom_() — it
    // triggers the first relayout().
    //
    // v2.17: LAZY image preload. Only preload the first N images (above
    // the fold). The rest are loaded on demand by the renderer (getImage
    // → loadImage on cache miss) when the user scrolls to them. This
    // reduces initial bandwidth + connection-pool pressure on image-
    // heavy pages (DDG results with 30+ thumbnails, YouTube search with
    // 20+ video thumbnails). The renderer's getImage() call already
    // triggers a background fetch on cache miss, so off-screen images
    // are simply not fetched until the user scrolls near them.
    {
        std::vector<std::string> imgUrls;
        std::function<void(const std::shared_ptr<Node>&)> walkImgs =
            [&](const std::shared_ptr<Node>& n) {
                for (auto& c : n->children) {
                    if (c->tag == "img") {
                        std::string u = pickImageSrc(c);
                        if (!u.empty()) imgUrls.push_back(u);
                    } else {
                        walkImgs(c);
                    }
                }
            };
        walkImgs(dom_);
        // v2.17: Only preload the first 10 images. The rest are loaded
        // on demand by the renderer when they come into view.
        constexpr size_t kMaxPreloadImages = 10;
        if (imgUrls.size() > kMaxPreloadImages) {
            std::cerr << "[img] lazy preload: " << kMaxPreloadImages
                      << " of " << imgUrls.size()
                      << " images (rest loaded on scroll)\n";
            imgUrls.resize(kMaxPreloadImages);
        }
        // Effective base (page URL, or <base href> when declared) so
        // images resolve like links/scripts/stylesheets do.
        ResourceLoader::instance().setBaseDir(effectiveBaseUrl_());
        ResourceLoader::instance().startPreload(imgUrls);
    }

    // Internal media (<video>/<audio>): collect this page's sources so
    // (a) players belonging to the PREVIOUS document can be pruned and
    // (b) the first relayout acquires fresh players for this one. Same
    // resolution rule layout will use (src attr, then <source src>).
    {
        std::unordered_set<std::string> pageMedia;
        std::function<void(const std::shared_ptr<Node>&)> walkMedia =
            [&](const std::shared_ptr<Node>& n) {
                for (auto& c : n->children) {
                    if (c->tag == "video" || c->tag == "audio") {
                        std::string s;
                        auto it = c->attrs.find("src");
                        if (it != c->attrs.end() && !it->second.empty())
                            s = it->second;
                        if (s.empty()) {
                            for (auto& sc : c->children) {
                                if (sc->tag == "source" &&
                                    sc->attrs.count("src")) {
                                    s = sc->attrs["src"];
                                    break;
                                }
                            }
                        }
                        if (!s.empty())
                            pageMedia.insert(
                                ResourceLoader::instance().resolve(s));
                    } else {
                        walkMedia(c);
                    }
                }
            };
        walkMedia(dom_);
        autoplayStarted_.clear();
        seekDragBox_ = -1;
        closeImageViewer_();
        media::prunePlayersExcept(pageMedia);
    }

    // v2.17: perf log now says "cssInline+imgPreload" (external CSS is
    // fetched in the background; first paint uses inline <style> only).
    std::cerr << "[perf] cssInline+imgPreload " << msSince(t0) << "ms\n";
    // v2.17: Store the inline styleText so pollCssArrival_ can append
    // external CSS to it when the background fetch completes.
    baseStyleText_ = styleText;
    baseCssRules_ = parseCSS(styleText);
    std::cerr << "[perf] parseCSS(" << styleText.size()/1024 << "KB) " << msSince(t0) << "ms\n";
    cssRules_ = baseCssRules_;
    applyZoom_();   // FIRST PAINT — text visible with inline CSS only

    // External JS: <script src="...">. The chunks are collected here but
    // executed AFTER the first frame reaches the screen (runPendingJs_,
    // driven by Browser::tick from the event loop) — first paint no longer
    // waits for the parallel JS fetch + duktape execution (which alone
    // burned up to the MB_JSBUDGET, default 1500 ms, on heavy pages).
    std::vector<JSScriptChunk> jsChunks;
    extractJSChunks(dom_, jsChunks);
    js_.setDocument(dom_);
    js_.setDocumentUrl(documentUrl_);
    if (!jsChunks.empty()) {
        pendingJs_ = std::move(jsChunks);
        hasDeferredJs_ = true;
    }
    frameDirty_ = true;
}

// dwm and similar minimal window managers render ONLY the window title
// in their bar — there is no URL field anywhere else on screen. Show
// "<doc title> — <address>" (or just the address when the page has no
// title) so the current location is always visible.
void Browser::updateWindowTitle_() {
    std::string t = findDocTitle(dom_);
    const std::string& url = currentPath_;
    // dwm hard-truncates the END of an overflowing bar — a long page
    // title (video pages ship 100+ char titles) pushes the URL out of
    // the bar entirely ("the url is not showing again"). Cap the title
    // (UTF-8 safe, no cut inside a multi-byte glyph) so the address
    // always survives on screen.
    const size_t kMaxTitle = 64;
    if (t.size() > kMaxTitle) {
        size_t cut = kMaxTitle;
        while (cut > 0 && cut < t.size() && (t[cut] & 0xC0) == 0x80) --cut;
        t = t.substr(0, cut) + "\xE2\x80\xA6";
    }
    std::string full = t.empty() ? url : (t + " \xE2\x80\x94 " + url);
    SDL_SetWindowTitle(win_, full.c_str());
}

// ---------------------------------------------------------------------------
// Frame scheduling (event-driven repaint + deferred JS)
// ---------------------------------------------------------------------------

// Runs the JS chunks collected by showPage_ AFTER the first frame is on
// screen: parallel-prefetch external sources, execute in document order
// under the time budget, then relayout once if anything actually ran.
void Browser::runPendingJs_() {
    if (!hasDeferredJs_) return;
    hasDeferredJs_ = false;
    std::vector<JSScriptChunk> jsChunks = std::move(pendingJs_);
    pendingJs_.clear();
    if (jsChunks.empty()) return;
    auto jsT0 = std::chrono::steady_clock::now();

    // Prefetch all external sources in parallel first (the walk below
    // then hits the fetch cache).
    {
        std::vector<std::string> urls;
        for (auto& ch : jsChunks)
            if (!ch.src.empty()) urls.push_back(ch.src);
        if (urls.size() >= 2) {
            unsigned hw = std::thread::hardware_concurrency();
            int n = (int)std::min<size_t>(urls.size(),
                                          std::min<size_t>(hw ? hw : 4u, 8u));
            std::atomic<size_t> next{0};
            auto worker = [&]() {
                for (;;) {
                    size_t i = next++;
                    if (i >= urls.size()) break;
                    if (isRemoteUrl(urls[i])) fetchUrlCached(urls[i], 10);
                }
            };
            std::vector<std::thread> pool;
            for (int t = 0; t < n; ++t) pool.emplace_back(worker);
            for (auto& th : pool) th.join();
        }
        // Resolve externals into bodies (document order preserved).
        for (auto& ch : jsChunks) {
            if (ch.src.empty()) continue;
            std::string resolved = resolveLink_(ch.src);
            std::string body;
            if (isRemoteUrl(resolved)) {
                FetchResult fr = fetchUrlCached(resolved);
                if (fr.ok) body = fr.body;
            } else {
                body = readFileToString(resolved);
            }
            if (body.empty())
                std::cerr << "[js] could not load " << resolved << "\n";
            ch.body = std::move(body);
        }
    }

    std::cout << "[Running JS] " << currentPath_ << "\n";
    // Time budget: heavy sites ship dozens of scripts whose tracking
    // payloads contribute nothing to the static render. Execute in
    // document-order chunks and stop once the budget is spent
    // (default 1500 ms, 0 = unlimited, MB_JSBUDGET overrides).
    int budgetMs = 1500;
    if (getenv("MB_JSBUDGET")) budgetMs = atoi(getenv("MB_JSBUDGET"));
    int ranScripts = 0, skippedScripts = 0;
    size_t okScripts = 0;
    for (auto& ch : jsChunks) {
        if (ch.body.empty()) continue;
        if (budgetMs > 0) {
            double spent = std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now() - jsT0)
                               .count();
            if (spent > budgetMs) { ++skippedScripts; continue; }
        }
        if (js_.execute(ch.body)) ++okScripts;
        ++ranScripts;
    }
    if (skippedScripts)
        std::cerr << "[js] budget: ran " << ranScripts << ", skipped "
                  << skippedScripts << " script(s) after " << budgetMs
                  << "ms\n";
    std::cerr << "[perf] deferredJS "
              << std::chrono::duration<double, std::milli>(
                     std::chrono::steady_clock::now() - jsT0).count()
              << "ms\n";
    // Only pay for a second full layout when a script actually executed
    // and may have mutated the DOM (pages whose bundles all fail to parse
    // — ES6+ on duktape — render identically without it).
    if (okScripts > 0) {
        relayout_();
        frameDirty_ = true;
    }
}

// One scheduler step, called once per event-loop iteration BEFORE the
// repaint decision: consumes the finished background document fetch,
// polls image arrivals, fires JS timers and, once the first frame has
// been presented, runs the deferred page scripts.
void Browser::tick() {
    // yt-dlp bridge finished? Continue on the main thread (open the
    // player page, or fall back to the normal page load with the reason
    // in the status bar). Results for superseded navigations are dropped.
    if (bridgePending_) pollBridgeResult_();

    // v2.17: External CSS fetch finished? Merge it with inline CSS,
    // re-parse, re-layout, repaint. This is the non-blocking CSS path
    // — first paint used inline CSS only; external CSS arrives here
    // ~100-300ms later and "snaps" the page into its styled form.
    if (cssSlot_ && cssSlot_->done.load(std::memory_order_acquire)) {
        pollCssArrival_();
    }

    // Navigation worker finished? Continue the load on the main thread
    // (DOM/CSS/layout/JS are main-thread-only state). The slot is
    // heap-owned and shared with the worker, so this is safe even when
    // the result outlives a superseded navigation (seq mismatch).
    if (navSeq_.load() > 0) {
        std::unique_ptr<FetchResult> fr;
        std::string orig;
        {
            std::lock_guard<std::mutex> lk(navSlot_->m);
            if (navSlot_->res &&
                navSlot_->doneSeq.load(std::memory_order_acquire) ==
                    navSeq_.load()) {
                fr = std::move(navSlot_->res);
                navSlot_->res.reset();
                orig = navOrigUrl_;
                navOrigUrl_.clear();
            }
        }
        if (fr) finishRemoteLoad_(std::move(*fr), orig);
    }

    // mpv liveness probe: give mpv ~2 s to get past stream resolution,
    // then either confirm playback or surface the reason it died (its
    // log tail) in the status bar — a silently vanishing player is the
    // classic "button does nothing" experience, so make failures loud.
    if (mpvCheckDueMs_ &&
        (int32_t)(SDL_GetTicks() - mpvCheckDueMs_) >= 0) {
        mpvCheckDueMs_ = 0;
        if (mpv::alive()) {
            pageInfo_ = "Playing in mpv";
        } else {
            std::string err = mpv::logTail(2);
            pageInfo_ = err.empty()
                ? "mpv exited immediately \xE2\x80\x94 see " + mpv::logPath()
                : "external player failed \xE2\x80\x94 " + err;
        }
        frameDirty_ = true;
    }

    // One synthetic test click once the page has settled: first paint
    // done AND the initial navigation consumed (the first paint is often
    // the bare "Loading…" page — clicking too early hits nothing).
    if (autoClickX_ >= 0 && !autoClickDone_ && paintedOnce_ &&
        (navSeq_.load() == 0 ||
         navSlot_->doneSeq.load(std::memory_order_acquire) ==
             navSeq_.load())) {
        if (autoClickDueMs_ == 0)
            autoClickDueMs_ = SDL_GetTicks() + 300;   // let images/JS settle
        else if ((int32_t)(SDL_GetTicks() - autoClickDueMs_) >= 0) {
            autoClickDone_ = true;
            std::cout << "[auto] firing click at " << autoClickX_ << ","
                      << autoClickY_ << "\n";
            handleClick(autoClickX_, autoClickY_);
        }
    }

    pollImageArrivals_();

    // Internal media: keep painting while a player is running (throttled
    // to ~30 fps — the decode thread publishes at the stream's own rate,
    // but the wall clock would otherwise wake the loop far faster than
    // any weak device can paint), and when a new frame landed anywhere
    // (paused preview arrival, seek result) so the UI tracks the decoder.
    if (media::anyPlaying()) {
        Uint32 now = SDL_GetTicks();
        if (now - lastMediaPaintMs_ >= 33) {
            lastMediaPaintMs_ = now;
            frameDirty_ = true;
        }
    }
    if (media::anyNewFrames(mediaSeenSeq_)) frameDirty_ = true;

    if (js_.pumpTimers()) frameDirty_ = true;   // timer may mutate the DOM
    if (hasDeferredJs_ && paintedOnce_) runPendingJs_();
}

// Images landed from the background pool since the last tick? Repaint
// now, and relayout debounced: image dimensions changed, so boxes must
// re-flow — but a gallery landing one-by-one must not trigger a full
// relayout per image on a weak device. One relayout per 300 ms window,
// plus a final one when the queue drains.
void Browser::pollImageArrivals_() {
    if (ResourceLoader::instance().consumeImagesArrived()) {
        frameDirty_ = true;
        imagesPendingRelayout_ = true;
        lastImageArrivalMs_ = SDL_GetTicks();
    }
    if (!imagesPendingRelayout_ || !dom_) return;
    bool drained = ResourceLoader::instance().pendingPreloads() == 0;
    bool quiet   = (SDL_GetTicks() - lastImageArrivalMs_) > 300;
    if (drained || quiet) {
        imagesPendingRelayout_ = false;
        relayout_();
        frameDirty_ = true;
    }
}

// v2.17: External CSS arrived from the background fetch thread.
// Merge it with the inline CSS (baseStyleText_), re-parse the combined
// stylesheet, re-layout, and trigger a repaint. This "snaps" the page
// from its unstyled (inline-CSS-only) first paint into its fully-styled
// form — the same progressive-rendering pattern Chrome/Firefox use.
void Browser::pollCssArrival_() {
    if (!cssSlot_) return;
    std::string externalCss;
    {
        std::lock_guard<std::mutex> lk(cssSlot_->m);
        if (!cssSlot_->done.load()) return;
        externalCss = std::move(cssSlot_->externalCss);
        cssSlot_->done.store(false);
    }
    // Cancel the slot so a stale worker can't double-fire.
    cssSlot_->cancelled.store(true);
    cssSlot_.reset();

    if (externalCss.empty() || !dom_) return;

    // Merge: inline CSS (baseStyleText_) + external CSS (just arrived).
    std::string combined = baseStyleText_ + externalCss;
    std::cerr << "[css] external CSS arrived (" << externalCss.size() / 1024
              << "KB) — re-parsing combined CSS ("
              << combined.size() / 1024 << "KB) + re-layout\n";
    auto t0 = std::chrono::steady_clock::now();
    baseCssRules_ = parseCSS(combined);
    cssRules_ = baseCssRules_;
    relayout_();
    frameDirty_ = true;
    std::cerr << "[perf] cssRestyle " << msSince(t0) << "ms\n";
}

// Hand a URL to mpv (external video player). mpv + yt-dlp plays YouTube
// watch URLs and every direct media format; the browser keeps showing the
// page (description, related links, comments) — playback lives in its own
// detached process, so the UI never blocks and closing the browser never
// kills a running video.
void Browser::maybePlayInMpv_(const std::string& url) {
    if (url.empty()) return;
    // A YouTube page that is NOT a specific video (home/search/channel):
    // point the user at a video first.
    if (shims::isYouTubeUrl(url) &&
        url.find("/watch") == std::string::npos &&
        url.find("youtu.be/") == std::string::npos &&
        url.find("/shorts/") == std::string::npos &&
        url.compare(0, 4, "mpv:") != 0) {
        pageInfo_ = "Open a video, then press v (or click its Play button).";
        frameDirty_ = true;
        return;
    }
    if (!mpv::available()) {
        pageInfo_ = "mpv not found \xE2\x80\x94 install mpv (and yt-dlp for YouTube)";
        frameDirty_ = true;
        return;
    }
    // YouTube watch URLs only play through mpv's ytdl_hook, which needs
    // yt-dlp (or youtube-dl) on PATH. Without it mpv exits within a
    // second with an opaque error — tell the user up front instead.
    if (mpv::needsYtDlp(url) && !mpv::ytDlpAvailable()) {
        pageInfo_ = "mpv needs yt-dlp for YouTube \xE2\x80\x94 install it "
                    "(e.g. pip install yt-dlp), then press v again";
        frameDirty_ = true;
        return;
    }
    if (mpv::play(url)) {
        pageInfo_ = "Starting mpv: " + url;
        // Schedule the liveness probe; nextWakeupMs() guarantees a tick
        // at that moment even if no OS events arrive.
        mpvCheckDueMs_ = SDL_GetTicks() + 2000;
    } else {
        pageInfo_ = "Failed to launch mpv";
    }
    frameDirty_ = true;
}

// ---------------------------------------------------------------------------
// yt-dlp bridge: YouTube watch URLs -> internal player (v2.5)
// ---------------------------------------------------------------------------

// Resolve `url` (a watch-style link — classification already done by the
// caller) into a progressive stream and play it on the built-in media
// page. The resolve runs on a detached thread like document fetches: a
// cold extraction spawns yt-dlp and does network round-trips (1-10 s),
// far too long to freeze the UI. Completion lands in the seq-guarded
// BridgeSlot and is consumed by pollBridgeResult_() from tick().
void Browser::openExtractedMedia_(const std::string& url) {
    currentPath_ = url;
    documentUrl_ = url;
    baseDir_ = url;

    // Cache hit (repeat visit / back-forward): no thread, no wait.
    extractor::ResolvedMedia rm;
    if (extractor::peek(url, rm)) {
        loadMediaPage_(url, rm.directUrl, rm.title, rm.via);
        return;
    }

    // v2.6 note: no yt-dlp on the machine is NO LONGER a dead end —
    // resolve() falls back to the Piped API, which needs no local
    // tooling. v2.8: when BOTH paths fail, the normal page-load path
    // renders the YouTube page from ytInitialData via the extractor in
    // app/shims.cpp (the Chrome UA in net/fetch.cpp guarantees the JSON
    // is in the page).

    // Screenshot/selftest tool: keep everything synchronous so the shot
    // is deterministic.
    if (syncNav_) {
        std::string err;
        if (extractor::resolve(url, rm, err)) {
            loadMediaPage_(url, rm.directUrl, rm.title, rm.via);
        } else {
            pageInfo_ = err;
            frameDirty_ = true;
            loadRemotePage_(url, err);
        }
        return;
    }

    pageInfo_ = extractor::available()
        ? "Resolving video stream (yt-dlp \xE2\x86\x92 Piped fallback)\xE2\x80\xA6"
        : "Resolving video stream (Piped)\xE2\x80\xA6";
    SDL_SetWindowTitle(win_, ("Resolving... " + url).c_str());
    frameDirty_ = true;

    int seq = ++bridgeGen_;
    bridgePending_ = true;
    auto slot = bridgeSlot_;   // heap-held: safe if the Browser dies first
    std::thread([slot, seq, url]() {
        extractor::ResolvedMedia rm;
        std::string err;
        bool ok = extractor::resolve(url, rm, err);
        BridgeResult r;
        r.ok = ok;
        r.originalUrl = url;
        r.directUrl = rm.directUrl;
        r.title = rm.title;
        r.ext = rm.ext;
        r.via = rm.via;
        r.err = err;
        std::lock_guard<std::mutex> lk(slot->m);
        slot->inbox[seq] = std::move(r);
    }).detach();
}

// Main-thread continuation of the bridge: adopt the finished resolve for
// the CURRENT generation and either open the player page or fall back to
// the normal page load. Stale generations (superseded while resolving)
// are dropped — an old result can never hijack a newer navigation.
void Browser::pollBridgeResult_() {
    BridgeResult r;
    {
        std::lock_guard<std::mutex> lk(bridgeSlot_->m);
        auto it = bridgeSlot_->inbox.find(bridgeGen_);
        if (it == bridgeSlot_->inbox.end()) {
            // Drop anything stale so the inbox cannot grow.
            if (!bridgeSlot_->inbox.empty())
                bridgeSlot_->inbox.clear();
            return;
        }
        r = std::move(it->second);
        bridgeSlot_->inbox.erase(it);
        bridgeSlot_->inbox.clear();
    }
    bridgePending_ = false;
    std::cout << "[extractor] " << (r.ok ? "resolved " : "FAILED ")
              << r.originalUrl;
    if (r.ok) std::cout << " -> " << r.directUrl.substr(0, 60) << "...";
    else      std::cout << " (" << r.err << ")";
    std::cout << "\n";
    if (r.ok) {
        loadMediaPage_(r.originalUrl, r.directUrl, r.title, r.via);
    } else {
        // The reason must survive the fallback page's own "HTTP 200 ..."
        // status update — finishRemoteLoad_ appends bridgeNote_ if set.
        // v2.8: the failure reason is also rendered into the YouTube
        // page as an amber "Video playback failed: <reason>" banner by
        // shims::youTubeLiteHtml (the bridgeReason argument).
        bridgeNote_ = "bridge: " + r.err;
        if (bridgeNote_.size() > 220)
            bridgeNote_ = bridgeNote_.substr(0, 220) + "...";
        pageInfo_ = bridgeNote_ + " \xE2\x80\x94 opening the page instead";
        loadRemotePage_(r.originalUrl, r.err);
    }
}

// How long the event loop may sleep before the browser wants a CPU tick:
// 0 when a frame is pending or deferred JS is ready to run, the JS timer
// deadline, the caret blink interval while editing, or -1 to sleep until
// the next OS event (idle = zero repaints = zero CPU).
int Browser::nextWakeupMs() const {
    if (frameDirty_) return 0;
    if (hasDeferredJs_) return paintedOnce_ ? 0 : 1;
    // A playing video wants repaints at its own frame rate — poll lightly
    // and let tick()'s throttle schedule the actual paints.
    if (media::anyPlaying()) return 5;
    // While a document fetches in the background, poll lightly so the
    // load completes promptly even with no OS events coming in. When the
    // result has landed but tick() has not consumed it yet, wake NOW:
    // a fast fetch (localhost, cache hit) can complete between loop
    // iterations, and falling through to the idle sleep here would leave
    // the page stuck on "Loading…" forever (no event will ever wake us).
    if (navSeq_.load() > 0) {
        if (navSlot_->doneSeq.load(std::memory_order_acquire) !=
            navSeq_.load())
            return 8;                 // fetch still running
        std::lock_guard<std::mutex> lk(navSlot_->m);
        if (navSlot_->res) return 0;  // result waiting: tick() must run
    }
    // While the image pool is draining, keep picking up arrivals.
    if (ResourceLoader::instance().pendingPreloads() > 0) return 40;
    // A yt-dlp resolve is running off-thread: poll lightly until its
    // result lands (typically a couple of seconds).
    if (bridgePending_) return 20;
    int t = js_.nextTimerDelayMs();
    bool caretAnimates = addressEditing_ || focusedNode_ != nullptr;
    if (caretAnimates) t = (t < 0) ? 450 : std::min(t, 450);
    // Pending mpv liveness probe: wake when it falls due.
    if (mpvCheckDueMs_) {
        int rem = (int)(mpvCheckDueMs_ - SDL_GetTicks());
        if (rem <= 0) return 0;
        t = (t < 0) ? rem : std::min(t, rem);
    }
    // Armed test click: poll until the first paint has happened.
    if (autoClickX_ >= 0 && !autoClickDone_ && paintedOnce_) return 30;
    return t;
}

bool Browser::needsRepaint() const { return frameDirty_; }

void Browser::loadLocalPage_(const std::string& absolutePath) {
    std::string html = readFileToString(absolutePath);
    if (html.empty()) {
        std::cerr << "[nav] could not open " << absolutePath << "\n";
        html = "<html><head><title>Not found</title></head>"
               "<body style='font-family: sans-serif'>"
               "<h1>Could not load page</h1>"
               "<p>The file <code>" + absolutePath + "</code> does not exist or"
               " could not be read.</p></body></html>";
    }
    size_t slash = absolutePath.find_last_of("/\\");
    std::string dir = (slash == std::string::npos) ? "" : absolutePath.substr(0, slash);
    focusedNode_.reset();
    carets_.clear();
    showPage_(html, absolutePath, dir);
    updateWindowTitle_();  // local pages never used to update the title
}

// Fetch the main document. For YouTube, v2.7 removed the lite shim
// entirely on the assumption that the Chrome UA alone would make YouTube
// render — but YouTube is a JS SPA, so the Chrome UA only got the SPA
// shell (887KB of HTML with 857KB of <script> and ~163 chars of visible
// text — just the footer).
//
// v2.8 brings back the ytInitialData / ytInitialPlayerResponse
// extractor in app/shims.cpp. With the Chrome UA the JSON is reliably
// present in the page, so the extractor works every time (in v2.6 it
// was flaky because the old "MiniBrowser" UA made YouTube serve a
// bot/consent stub that often had no JSON). The page is now branded
// just "YouTube" (not "YouTube Lite") because it is NOT a lite
// alternative site — it's the actual page's own data, rendered
// statically because the JS engine (Duktape ES5.1) cannot drive the SPA.
//
// v2.8 also: run the extractor EVEN when the fetch failed (fr.ok=false).
// YouTube often serves a 429 + redirect to google.com/sorry/ to
// datacenter IPs (this sandbox is one), which makes fr.ok=false and the
// v2.6/v2.7 code never entered the extractor — the user saw a network
// error page instead of the YouTube fallback page. The extractor's
// watch-page branch only needs the videoId from the URL to render a
// playable page (Play-in-browser chip + "metadata could not be read"
// message), so it works even on a 429 /sorry/ body. Home / search pages
// on a 429 body still produce the "category shortcuts" or "try again"
// fallback because they don't depend on the body content.
//
// Runs on the nav worker thread — no Browser state is touched here.
// shims::youTubeLiteHtml builds its own duktape heap per call and shares
// nothing mutable, so it is thread-safe off the UI thread.
// `bridgeReason` is stamped into the YouTube page as an amber banner
// ("Video playback failed: ...") when the bridge just failed.
static FetchResult fetchDocumentWithShims(const std::string& url,
                                          const std::string& bridgeReason) {
    FetchResult fr = fetchUrl(url);
    // v2.8: also run the YouTube extractor on fetch failures. The
    // watch-page branch only needs the videoId from the URL, so it can
    // render a playable page even when YouTube served a 429 /sorry/
    // body. (YouTube bot-gates datacenter IPs aggressively; this
    // sandbox is on a datacenter IP, so watch URLs always hit this path
    // here. On a residential IP the body would have the real
    // ytInitialPlayerResponse and the extractor would surface the
    // title / description / related videos too.)
    if (shims::isYouTubeUrl(url)) {
        bool ok = false;
        std::string extracted =
            shims::youTubeLiteHtml(url, fr.body, ok, bridgeReason);
        if (ok && !extracted.empty()) {
            std::cerr << "[yt] rendered YouTube page from ytInitialData ("
                      << extracted.size() / 1024 << "KB html)"
                      << (fr.ok ? "" : " [fetch had failed: "
                                + fr.error + "]")
                      << (bridgeReason.empty() ? "" : " [bridge failed]")
                      << "\n";
            fr.body = extracted;
            fr.contentType = "text/html; charset=utf-8";
            // Promote the result to "ok" so the rest of the pipeline
            // (finishRemoteLoad_, render path) renders our extracted
            // body instead of the network-error page. The HTTP status
            // is preserved in fr.status for the status bar.
            if (!fr.ok) {
                fr.ok = true;
                if (fr.error.empty()) fr.error = "extracted from raw page";
            }
        } else if (fr.ok) {
            // ytInitialData was missing or unreadable — keep the raw HTML
            // so the user at least sees something. With the Chrome UA in
            // net/fetch.cpp this branch should be rare.
            std::cerr << "[yt] extractor returned no body; keeping raw "
                      << fr.body.size() / 1024 << "KB HTML\n";
        } else {
            // Fetch failed AND the extractor couldn't salvage anything
            // (e.g. not a watch URL, or MB_NOSHIM=1). Fall through to
            // the network-error path in finishRemoteLoad_.
            std::cerr << "[yt] fetch failed (" << fr.error
                      << ") and extractor returned nothing\n";
        }
    }
    return fr;
}

void Browser::loadRemotePage_(const std::string& url,
                              const std::string& bridgeReason) {
    // v2.8: maybeRewriteSearchUrl is still a no-op (returns ""), so
    // google.com/search loads Google directly. The Chrome UA + Sec-Fetch-*
    // headers in net/fetch.cpp make Google serve its real search page;
    // note however that Google's real search page is a JS SPA, so it
    // still renders mostly blank in our engine. The user explicitly asked
    // NOT to rewrite google.com/search, so we honour that. Address-bar
    // search phrases (the other entry point) go to html.duckduckgo.com/html
    // — the actual server-rendered DDG — which DOES render in the engine.
    std::string fetchUrlStr = url;
    std::string rewritten = shims::maybeRewriteSearchUrl(url);
    if (!rewritten.empty()) fetchUrlStr = rewritten;

    currentPath_ = fetchUrlStr;
    documentUrl_ = fetchUrlStr;
    baseDir_ = fetchUrlStr;
    pageInfo_ = "Loading\xE2\x80\xA6";
    SDL_SetWindowTitle(win_, ("Loading... " + fetchUrlStr).c_str());
    frameDirty_ = true;   // show the Loading status right now

    if (syncNav_) {
        // --screenshot mode: blocking load keeps that tool trivial.
        finishRemoteLoad_(fetchDocumentWithShims(fetchUrlStr, bridgeReason),
                          url);
        return;
    }
    startAsyncFetch_(fetchUrlStr, url, bridgeReason);
}

// Spawn the network fetch on a worker thread. The closure copies
// everything it needs and never touches DOM/CSS/layout/JS (main-thread
// state); completion is communicated through the seq-guarded slot that
// tick() polls. A navigation started while another is in flight bumps
// navSeq_, so the stale result is discarded on arrival.
void Browser::startAsyncFetch_(const std::string& fetchUrlStr,
                               const std::string& originalUrl,
                               const std::string& bridgeReason) {
    navOrigUrl_ = originalUrl;
    int seq = navSeq_.fetch_add(1) + 1;
    // Heap-owned slot shared with the worker: safe even if the user
    // quits (and the Browser is destroyed) while the fetch is pending —
    // the worker writes into a slot nobody will read, never into `this`.
    auto slot = navSlot_;
    std::thread([slot, seq, fetchUrlStr, bridgeReason]() {
        FetchResult fr = fetchDocumentWithShims(fetchUrlStr, bridgeReason);
        releaseThreadCurl();   // short-lived thread: release its handle
        {
            std::lock_guard<std::mutex> lk(slot->m);
            slot->res = std::make_unique<FetchResult>(std::move(fr));
            slot->doneSeq.store(seq, std::memory_order_release);
        }
    }).detach();
}

// Main-thread continuation of a remote load: runs once the document has
// been fetched (on either the sync or the async path). `url` is the
// pre-shim original, used for the history title and error pages.
void Browser::finishRemoteLoad_(FetchResult fr, const std::string& url) {
    rawSource_ = fr.body;

    std::string info = (fr.ok ? "HTTP " + std::to_string(fr.status)
                              : "FAILED: " + fr.error);
    if (!fr.contentType.empty()) info += " · " + fr.contentType;
    if (fr.ok) {
        char sz[32];
        snprintf(sz, sizeof(sz), "%.1f KB", fr.body.size() / 1024.0);
        info += " · " + std::string(sz);
    }
    pageInfo_ = info;
    // Carry the yt-dlp failure reason across the fallback load so the
    // user still sees it after "HTTP 200" replaces the loading status.
    if (!bridgeNote_.empty()) {
        info += " \xC2\xB7 " + bridgeNote_;
        bridgeNote_.clear();
        pageInfo_ = info;
    }

    if (!fr.ok && fr.body.empty()) {
        // Network-level failure (nothing came back): a styled local error
        // page, like a browser's dinosaur.
        std::string safe = fr.error;
        std::string html =
            "<html><head><title>Error</title></head>"
            "<body style='font-family: sans-serif'>"
            "<div style='background: #fdecea; border: 1px solid #d93025; "
            "padding: 12px; margin: 16px'>"
            "<h2 style='font-size: 20px'>Can't reach this page</h2>"
            "<p><b>" + url + "</b></p>"
            "<p>" + safe + "</p>"
            "<p style='font-size: 13px; color: #555'>Check the address, your "
            "network connection, or try again later.</p>"
            "</div></body></html>";
        docBaseUrl_.clear();   // no <base> processing on synthetic pages
        dom_ = parseHTML(html);
        baseCssRules_ = parseCSS("");
        cssRules_ = baseCssRules_;
        applyZoom_();
        js_.setDocument(dom_);
        js_.setDocumentUrl("");
        relayout_();
        SDL_SetWindowTitle(win_, ("Error \xE2\x80\x94 " + url).c_str());
        frameDirty_ = true;
        return;
    }
    // HTTP error status WITH a body (403 block pages, 404s, 5xx): render
    // the body like real browsers do — Reddit/Cloudflare ship full HTML
    // with their error statuses.

    documentUrl_ = fr.finalUrl.empty() ? url : fr.finalUrl;
    baseDir_ = documentUrl_;

    std::string title = documentUrl_;
    std::string html = fr.body;

    // Non-HTML content: render text/plain as <pre>, images as a page
    // with just the image, everything else as a small notice.
    bool isHtml = fr.contentType.empty() ||
                  fr.contentType.find("html") != std::string::npos ||
                  fr.contentType.find("xml") != std::string::npos;
    bool isImage = fr.contentType.compare(0, 6, "image/") == 0;
    bool isText  = fr.contentType.compare(0, 5, "text/") == 0;
    if (isImage) {
        html = "<html><head><title>" + title + "</title></head>"
               "<body><img src=\"" + documentUrl_ + "\"></body></html>";
    } else if (!isHtml && isText) {
        html = "<html><head><title>" + title + "</title></head>"
               "<body><pre>" + fr.body + "</pre></body></html>";
    } else if (!isHtml) {
        html = "<html><head><title>" + title + "</title></head>"
               "<body><h2>Unsupported content type</h2>"
               "<p>" + fr.contentType + "</p></body></html>";
    }

    focusedNode_.reset();
    carets_.clear();
    showPage_(html, documentUrl_, documentUrl_);
    updateWindowTitle_();
    return;
}

// Built-in pages -----------------------------------------------------------

static std::string aboutHomeHtml() {
    return R"HTML(<html><head><title>MiniBrowser Home</title></head>
<body style="font-family: sans-serif">
<div style="background: #1a4fa0; padding: 24px">
  <h1 style="font-size: 36px; color: #ffffff">MiniBrowser 2.13</h1>
  <p style="color: #cfe0ff">A tiny SDL2 browser with real HTTP/HTTPS, a Duktape
  JavaScript engine and CSS styling. Round 16: YouTube search now uses
  DuckDuckGo with <code>site:youtube.com</code> filter. v2.12 fixed the
  buzz sound by switching audio output from S16 to F32 (FFmpeg decoders
  produce float natively; the S16 quantization was the buzz source).
  v2.13 fixes the YouTube search: typing in the YouTube search box used
  to submit to <code>youtube.com/results</code> which is a JS-heavy SPA
  whose <code>ytInitialData</code> is served inconsistently — the
  extractor often found 0 videos and showed "YouTube returned no
  readable results". Now the search box submits to
  <code>html.duckduckgo.com/html</code> with a
  <code>site:youtube.com</code> filter, so results are restricted to
  YouTube pages but rendered as plain HTML links that work in our
  engine. Clicking a result navigates to the watch page, which the
  yt-dlp bridge resolves to a playable stream. On top of v2.12's F32
  audio, v2.11's lazy swr init, v2.10's yt-dlp auto-cookie, v2.9's full
  Chrome client-hints, v2.8's hybrid rendering, v2.7's Chrome UA, and
  the internal media stack (&lt;video&gt; and &lt;audio&gt; decode
  in-process via FFmpeg).</p>
</div>
<h2>Try it out</h2>
<ul>
  <li><a href="https://example.com">https://example.com</a> — real HTTPS fetch</li>
  <li><a href="https://www.youtube.com">https://www.youtube.com</a> — YouTube home (data-extractor renders the search box + category shortcuts)</li>
  <li><a href="https://www.youtube.com/results?search_query=hello">YouTube search</a> — renders real results from ytInitialData JSON</li>
  <li><a href="https://www.youtube.com/watch?v=dQw4w9WgXcQ">YouTube watch</a> — Piped API resolves the stream URL → in-browser media player</li>
  <li><a href="media.html">media.html</a> — internal video + audio player demo</li>
  <li><a href="test.html">test.html</a> — the local feature tour</li>
  <li><a href="page2.html">page2.html</a> — a second local page</li>
  <li><a href="#top">Jump to top (#fragment link)</a></li>
  <li><a href="view-source:test.html">view-source:test.html</a> — Ctrl+U anywhere</li>
</ul>
<p>Tip: click any image to open it in the zoom/pan viewer; click a video to
play it right on the page. Paste a YouTube watch link to play it internally
(needs <b>yt-dlp</b>: <code>pip install yt-dlp</code>; progressive quality).
Type a phrase like "hello world" in the address bar to search DuckDuckGo
(server-rendered HTML view). Cookies are persisted at
<code>$HOME/.cache/minibrowser/cookies.txt</code> — delete that file to
clear your session.</p>
<h2>Keyboard shortcuts</h2>
<p>Ctrl+L address bar · Ctrl+F find · Ctrl++/− zoom · Ctrl+U view source ·
Ctrl+S save page · Ctrl+D bookmark · Ctrl+B bookmarks bar · Ctrl+C copy
selection · F5 reload · F11 fullscreen · Alt+←/→ back/forward · Esc close/quit</p>
<h2>JavaScript</h2>
<p id="jsline" style="background: #eef; padding: 8px">JS running...</p>
<form onsubmit="alert('Form submitted with q=' + document.getElementById('q').value); return false;">
  <input id="q" value="hello" style="width: 140px">
  <input type="submit" value="Try alert()">
</form>
<script>
  document.getElementById('jsline').textContent =
    'Hello from Duktape! 6*7=' + (6*7) + ', date via JS: ' + new Date().toString();
  setTimeout(function () {
    document.getElementById('jsline').classList.add('ok');
    document.getElementById('jsline').style.background = '#dfd';
  }, 400);
</script>
</body></html>)HTML";
}

void Browser::loadAboutPage_(const std::string& what) {
    if (what == "home" || what.empty()) {
        focusedNode_.reset();
        carets_.clear();
        showPage_(aboutHomeHtml(), "about:home", "");
        updateWindowTitle_();
    } else {  // about:blank
        focusedNode_.reset();
        carets_.clear();
        showPage_("<html><head><title>Blank page</title></head><body></body></html>",
                  "about:blank", "");
        updateWindowTitle_();
    }
}

void Browser::loadViewSource_(const std::string& target) {
    std::string src;
    if (target == "current" || target.empty()) {
        src = rawSource_;
    } else {
        std::string resolved = resolveLink_(target);
        if (isRemoteUrl(resolved)) {
            FetchResult fr = fetchUrlCached(resolved);
            src = fr.ok ? fr.body : ("// fetch failed: " + fr.error);
        } else {
            src = readFileToString(stripFileQueryFrag(resolved));
        }
    }
    // Escape it and show inside a <pre>.
    std::string esc;
    for (char c : src) {
        switch (c) {
            case '&':  esc += "&amp;";  break;
            case '<':  esc += "&lt;";   break;
            case '>':  esc += "&gt;";   break;
            default:   esc += c;
        }
    }
    std::string html =
        "<html><head><title>View source</title></head>"
        "<body style='background: #fafafa'><p style='font-family: monospace; "
        "color: #666'>Source of: <b>" + (target.empty() ? currentPath_ : target) +
        "</b></p><pre>" + esc + "</pre></body></html>";
    focusedNode_.reset();
    carets_.clear();
    showPage_(html, "view-source:" + (target.empty() ? currentPath_ : target), "");
    updateWindowTitle_();
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

void Browser::relayout_() {
    // Push the hovered node into the layout so CSS :hover selectors match.
    setHoveredNode(hoveredNode_);
    layout_ = layout(dom_, contentW(), font_, cssRules_, baseDir_);
    scrollY_ = std::min(scrollY_, maxScroll());
    // Media boxes (<video>/<audio>): make sure a player exists for each
    // and that loop/muted/autoplay attributes are reflected. acquirePlayer
    // is a registry lookup after the first call, so hover/zoom relayouts
    // stay cheap.
    applyAttrsToPlayers_();
}

void Browser::applyAttrsToPlayers_() {
    for (auto& b : layout_.boxes) {
        if (!b.isVideo && !b.isAudio) continue;
        if (b.mediaPath.empty()) continue;
        auto p = media::acquirePlayer(b.mediaPath);
        if (!p) continue;
        auto node = b.sourceNode.lock();
        if (node) {
            if (node->attrs.count("loop"))  p->setLoop(true);
            if (node->attrs.count("muted")) p->setMuted(true);
            if (node->attrs.count("autoplay") &&
                !autoplayStarted_.count(b.mediaPath)) {
                autoplayStarted_.insert(b.mediaPath);
                p->play();
            }
        }
    }
}

// Chrome-style built-in media page: navigating to a media file/URL no
// longer forks mpv — the video plays INSIDE the browser window through
// the normal layout + renderer + media pipeline. mpv stays reachable via
// the mpv: link on this page (and the v key) as an explicit escape hatch.
//
// srcOverride/mediaTitle (yt-dlp bridge form): the page URL keeps the
// ORIGINAL watch link for the address bar / history / mpv: hatch, while
// the <video> src points at the extracted progressive stream and the
// caption shows the video title.
void Browser::loadMediaPage_(const std::string& url,
                             const std::string& srcOverride,
                             const std::string& mediaTitle,
                             const std::string& via) {
    bool bridged = !srcOverride.empty();
    // Local files must be anchored to an ABSOLUTE path: the page's base
    // is the file's directory, and a relative src resolved against the
    // FILE path produced "dir/file.mp4dir/file.mp4" joins. (Bridge mode
    // is remote-only; the src is already absolute.)
    std::string srcUrl = bridged ? srcOverride : url;
    std::string base = url;
    if (!bridged && !isRemoteUrl(url)) {
        std::string abs = url;
        if (!isAbsolutePath(url)) {
            char buf[4096];
            if (::realpath(url.c_str(), buf)) abs = buf;
            else                            abs = normalizePath(url);
        }
        srcUrl = abs;
        size_t slash = abs.find_last_of("/");
        base = (slash == std::string::npos) ? std::string(".")
                                            : abs.substr(0, slash);
    }
    std::string esc;        // display/link form (the original URL)
    std::string escSrc;     // src-attribute form (the actual stream/local path)
    std::string escTitle;   // text/title-tag form of the video title
    auto escapeInto = [](const std::string& in, std::string& out) {
        for (char c : in) {
            switch (c) {
                case '&': out += "&amp;"; break;
                case '<': out += "&lt;";  break;
                case '>': out += "&gt;";  break;
                case '"': out += "%22";   break;
                default:  out += c;
            }
        }
    };
    escapeInto(url, esc);
    escapeInto(srcUrl, escSrc);
    escapeInto(bridged && !mediaTitle.empty() ? mediaTitle : "Media player",
               escTitle);
    // autoplay+muted mirrors Chrome's autoplay policy: playback starts
    // immediately but silent until the user unmutes via the mute button.
    std::string html =
        "<html><head><title>" + escTitle + "</title></head>"
        "<body style='background:#101010; margin:0'>"
        "<div style='text-align:center; margin-top:48px'>"
        "<video src=\"" + escSrc + "\" controls autoplay muted "
        "style=\"width:92%\"></video>"
        "<p style='color:#e8ebf2; font-size:15px; font-weight:bold'>" +
        escTitle + "</p>"
        "<p style='color:#8a93a5; font-size:12px'>MiniBrowser player "
        "\xE2\x80\x94 " + esc +
        (bridged ? " \xC2\xB7 stream resolved via " +
                          (via.empty() ? std::string("the bridge") : via)
                   : "") + "</p>"
        "<p style='font-size:12px'><a style='color:#7aa7c7' href=\"mpv:" +
        esc + "\">play in external mpv instead</a></p>"
        "</div></body></html>";
    focusedNode_.reset();
    carets_.clear();
    showPage_(html, url, base);
    updateWindowTitle_();
}

// ---------------------------------------------------------------------------
// Image viewer overlay (click a plain <img> to inspect it)
// ---------------------------------------------------------------------------

void Browser::openImageViewer_(const std::string& url) {
    viewerOpen_ = true;
    viewerUrl_ = url;
    viewerZoom_ = 1.0f;
    viewerPanX_ = viewerPanY_ = 0;
    viewerPanning_ = false;
    std::cout << "[viewer] open " << url << "\n";
}

void Browser::closeImageViewer_() {
    viewerOpen_ = false;
    viewerPanning_ = false;
}

bool Browser::viewerKey_(int sym, Uint16 mod) {
    (void)mod;
    switch (sym) {
        case SDLK_ESCAPE:
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
            closeImageViewer_();
            return true;
        case SDLK_PLUS:
        case SDLK_EQUALS:
        case SDLK_KP_PLUS:
            viewerZoom_ = std::min(12.0f, viewerZoom_ * 1.2f);
            return true;
        case SDLK_MINUS:
        case SDLK_KP_MINUS:
            viewerZoom_ = std::max(0.05f, viewerZoom_ / 1.2f);
            return true;
        case SDLK_0:
        case SDLK_KP_0:
            viewerZoom_ = 1.0f;
            viewerPanX_ = viewerPanY_ = 0;
            return true;
        case SDLK_LEFT:  viewerPanX_ += 40; return true;
        case SDLK_RIGHT: viewerPanX_ -= 40; return true;
        case SDLK_UP:    viewerPanY_ += 40; return true;
        case SDLK_DOWN:  viewerPanY_ -= 40; return true;
        default:
            return true;   // modal: swallow everything else
    }
}

void Browser::drawImageViewer_() {
    int cx = contentX(), cy = contentY(), cw = contentW(), ch = contentH();

    // Backdrop.
    SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(ren_, 8, 8, 10, 235);
    SDL_Rect bd = {cx, cy, cw, ch};
    SDL_RenderFillRect(ren_, &bd);
    SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_NONE);

    // Close button, top-right of the content area.
    viewerCloseRect_ = {cx + cw - 34, cy + 6, 28, 28};
    drawButton_(viewerCloseRect_.x, viewerCloseRect_.y,
                viewerCloseRect_.w, viewerCloseRect_.h, "\xC3\x97", true,
                hitButton_(viewerCloseRect_.x, viewerCloseRect_.y,
                           viewerCloseRect_.w, viewerCloseRect_.h,
                           mouseX_, mouseY_));

    // The image, centered, fit-to-window at zoom 100%.
    CachedImage* img = viewerUrl_.empty() ? nullptr : getImage(viewerUrl_);
    if (img && img->surface && img->w > 0 && img->h > 0) {
        float fit = std::min((float)cw / img->w, (float)ch / img->h);
        if (fit > 1.0f) fit = 1.0f;
        float scale = fit * viewerZoom_;
        int dw = std::max(1, (int)(img->w * scale));
        int dh = std::max(1, (int)(img->h * scale));
        SDL_Texture* tex =
            sharedTextureFor(ren_, "viewer:" + viewerUrl_, img->surface);
        if (tex) {
            SDL_Rect dst = {cx + (cw - dw) / 2 + viewerPanX_,
                            cy + (ch - dh) / 2 + viewerPanY_, dw, dh};
            SDL_RenderCopy(ren_, tex, nullptr, &dst);
        }
        char zbuf[16];
        snprintf(zbuf, sizeof zbuf, "%d%%",
                 (int)(viewerZoom_ * 100.0f + 0.5f));
        std::string hint = std::string(zbuf) +
            "  \xC2\xB7  scroll = zoom  \xC2\xB7  drag = pan  \xC2\xB7  Esc closes";
        int hw = 0, hh = 0;
        SDL_Texture* t = cachedTextTexture(ren_, font_, hint,
                                           {150, 150, 155, 255}, &hw, &hh);
        if (t) {
            SDL_Rect hd = {cx + 8, cy + ch - hh - 6, hw, hh};
            SDL_RenderCopy(ren_, t, nullptr, &hd);
        }
    } else {
        std::string msg = "Cannot display image";
        int tw = 0, th = 0;
        SDL_Texture* t = cachedTextTexture(ren_, font_, msg,
                                           {180, 180, 180, 255}, &tw, &th);
        if (t) {
            SDL_Rect dst = {cx + (cw - tw) / 2, cy + (ch - th) / 2, tw, th};
            SDL_RenderCopy(ren_, t, nullptr, &dst);
        }
    }
}

int Browser::maxScroll() const {
    return std::max(0, layout_.contentHeight - contentH());
}

void Browser::applyZoom_() {
    // Zoom is now applied inside computeStyle via a global scale, so it
    // also affects tag-default sizes (h1-h6) — the old approach only
    // scaled CSS-declared font-size values.
    setGlobalZoom(zoom_);
    if (dom_) relayout_();
}

// ---------------------------------------------------------------------------
// Navigation API
// ---------------------------------------------------------------------------

void Browser::navigate(const std::string& rawTarget) {
    std::string target = rawTarget;
    // Trim whitespace.
    size_t a = target.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return;
    size_t b = target.find_last_not_of(" \t\r\n");
    target = target.substr(a, b - a + 1);

    // v2.16: DuckDuckGo redirect URL unwrapping. DDG result links use
    // the format:
    //   https://duckduckgo.com/l/?uddg=<URL-encoded-target>&rut=<token>
    // When clicked, DDG's server returns a 200 with a JS redirect page:
    //   <script>window.parent.location.replace("<target>")</script>
    //   <noscript><meta http-equiv='refresh' content='0;URL=<target>'></noscript>
    // Our browser doesn't support window.parent.location.replace() or
    // meta refresh, so the user sees a blank page instead of the target
    // site. Fix: detect the DDG redirect URL, decode the 'uddg' parameter,
    // and navigate directly to the target site — bypassing DDG's redirect
    // endpoint entirely.
    {
        size_t ddgPos = target.find("duckduckgo.com/l/");
        if (ddgPos != std::string::npos) {
            size_t qPos = target.find("uddg=", ddgPos);
            if (qPos != std::string::npos) {
                size_t vStart = qPos + 5;  // length of "uddg="
                size_t vEnd = target.find('&', vStart);
                if (vEnd == std::string::npos) vEnd = target.size();
                std::string encoded = target.substr(vStart, vEnd - vStart);
                std::string decoded = urlDecode(encoded);
                if (!decoded.empty() &&
                    (decoded.compare(0, 7, "http://") == 0 ||
                     decoded.compare(0, 8, "https://") == 0)) {
                    std::cerr << "[nav] DDG redirect unwrapped: "
                              << target.substr(0, 60) << "..."
                              << " -> " << decoded.substr(0, 80) << "\n";
                    target = decoded;
                }
            }
        }
    }

    // mpv: pseudo-scheme — the YouTube watch pages carry a "Play in mpv"
    // button linking here. Hands the URL to mpv (yt-dlp resolves YouTube
    // streams); the browser stays where it is.
    if (target.compare(0, 4, "mpv:") == 0) {
        maybePlayInMpv_(target.substr(4));
        return;
    }
    // Direct media links (.mp4/.webm/...): play them INSIDE the browser
    // via the built-in media page (the old path forked mpv). Never
    // download the whole file just to print "Unsupported content type".
    if (isRemoteUrl(target) && media::isMediaUrl(target)) {
        if (historyIndex_ + 1 < history_.size())
            history_.resize(historyIndex_ + 1);
        history_.push_back({target, 0});
        historyIndex_ = history_.size() - 1;
        scrollY_ = 0;
        loadMediaPage_(target);
        return;
    }

    // Smart address bar: bare domains get an https:// prefix.
    if (target != "about:home" && target != "about:blank" &&
        target.compare(0, 12, "view-source:") != 0) {
        target = normalizeAddressInput(target);
    }

    // Web search from the address bar: a phrase that is neither a URL,
    // a bare domain nor a file path goes to the actual server-rendered
    // DuckDuckGo at html.duckduckgo.com/html (v2.8 — v2.7 used the
    // duckduckgo.com JS SPA which rendered blank; v2.6 used the
    // lite.duckduckgo.com/lite endpoint which the user asked to remove).
    // html.duckduckgo.com/html is NOT the "lite" alternative — it's the
    // same DuckDuckGo service, just server-rendered HTML (no JS required),
    // so the engine renders the results list directly.
    if (target != "about:home" && target != "about:blank" &&
        target.compare(0, 12, "view-source:") != 0 &&
        shims::looksLikeSearchQuery(target)) {
        target = shims::webSearchUrl(target);
    }

    // YouTube watch-style URLs: resolve to a direct stream with yt-dlp
    // and play INSIDE the browser (media::extractor). The history entry
    // keeps the original watch URL; back/forward re-resolves instantly
    // from the extractor cache. When no resolver exists or extraction
    // fails, the normal page load takes over below — v2.8 renders the
    // YouTube page from ytInitialData (the Chrome UA in net/fetch.cpp
    // guarantees the JSON is in the page), so the user always sees a
    // clean page with the title / description / related videos.
    if (isRemoteUrl(target) && media::extractor::isExtractableUrl(target)) {
        if (historyIndex_ + 1 < history_.size())
            history_.resize(historyIndex_ + 1);
        history_.push_back({target, 0});
        historyIndex_ = history_.size() - 1;
        scrollY_ = 0;
        openExtractedMedia_(target);
        return;
    }

    // Remember where the user was on the CURRENT page before leaving.
    if (!history_.empty() && historyIndex_ < history_.size())
        history_[historyIndex_].scrollY = scrollY_;

    if (target == "about:home" || target == "about:blank") {
        // Truncate any forward history when navigating from a back-state.
        if (historyIndex_ + 1 < history_.size())
            history_.resize(historyIndex_ + 1);
        history_.push_back({target, 0});
        historyIndex_ = history_.size() - 1;
        scrollY_ = 0;
        loadAboutPage_(target == "about:home" ? "home" : "blank");
        return;
    }
    if (target.compare(0, 12, "view-source:") == 0) {
        std::string inner = target.substr(12);
        if (historyIndex_ + 1 < history_.size())
            history_.resize(historyIndex_ + 1);
        history_.push_back({target, 0});
        historyIndex_ = history_.size() - 1;
        scrollY_ = 0;
        loadViewSource_(inner);
        return;
    }
    if (isRemoteUrl(target)) {
        if (historyIndex_ + 1 < history_.size())
            history_.resize(historyIndex_ + 1);
        history_.push_back({target, 0});
        historyIndex_ = history_.size() - 1;
        scrollY_ = 0;
        loadRemotePage_(target);
        return;
    }

    // Local file.
    std::string path = stripFileQueryFrag(isAbsolutePath(target)
                                              ? target : resolveUrl_(target));
    // Local media file (address bar, drag & drop, file manager): internal
    // player page instead of "Unsupported content type".
    if (media::isMediaUrl(path)) {
        if (historyIndex_ + 1 < history_.size())
            history_.resize(historyIndex_ + 1);
        history_.push_back({path, 0});
        historyIndex_ = history_.size() - 1;
        scrollY_ = 0;
        loadMediaPage_(path);
        return;
    }
    if (historyIndex_ + 1 < history_.size())
        history_.resize(historyIndex_ + 1);
    history_.push_back({path, 0});
    historyIndex_ = history_.size() - 1;
    scrollY_ = 0;
    loadLocalPage_(path);
}

void Browser::back() {
    if (!canGoBack()) return;
    if (historyIndex_ < history_.size())
        history_[historyIndex_].scrollY = scrollY_;
    --historyIndex_;
    loadHistoryEntry_(history_[historyIndex_]);
}

void Browser::forward() {
    if (!canGoForward()) return;
    if (historyIndex_ < history_.size())
        history_[historyIndex_].scrollY = scrollY_;
    ++historyIndex_;
    loadHistoryEntry_(history_[historyIndex_]);
}

void Browser::reload() {
    if (history_.empty()) return;
    clearFetchCache();   // documents should be re-fetched fresh
    loadHistoryEntry_(history_[historyIndex_]);
}

// Re-load a history entry without touching historyIndex_.
void Browser::loadHistoryEntry_(const HistoryEntry& e) {
    scrollY_ = 0;
    if (e.url == "about:home")       loadAboutPage_("home");
    else if (e.url == "about:blank") loadAboutPage_("blank");
    else if (e.url.compare(0, 12, "view-source:") == 0)
        loadViewSource_(e.url.substr(12) == currentPath_ ? "" : e.url.substr(12));
    else if (isRemoteUrl(e.url) && media::extractor::isExtractableUrl(e.url))
        openExtractedMedia_(e.url);   // bridge: cache peek makes this instant
    else if (media::isMediaUrl(e.url) &&
             (isRemoteUrl(e.url) || ResourceLoader::fileExists(e.url)))
        loadMediaPage_(e.url);
    else if (isRemoteUrl(e.url))     loadRemotePage_(e.url);
    else                             loadLocalPage_(e.url);
    // Restore the reading position saved when we left this page.
    scrollY_ = std::clamp(e.scrollY, 0, maxScroll());
}

// ---------------------------------------------------------------------------
// Fragment links (#anchor) and forms
// ---------------------------------------------------------------------------

void Browser::scrollToFragment_(const std::string& frag) {
    if (frag.empty()) { scrollY_ = 0; return; }
    auto target = findById(dom_, frag);
    if (!target) {
        // Also match <a name="...">.
        std::function<std::shared_ptr<Node>(const std::shared_ptr<Node>&)> findName =
            [&](const std::shared_ptr<Node>& n) -> std::shared_ptr<Node> {
                for (auto& c : n->children) {
                    if (c->tag == "a" && c->attrs.count("name") &&
                        c->attrs["name"] == frag) return c;
                    if (auto r = findName(c)) return r;
                }
                return nullptr;
            };
        target = findName(dom_);
    }
    if (!target) return;
    // Find the box belonging to this node and scroll to it.
    for (auto& b : layout_.boxes) {
        if (b.sourceNode.lock() == target) {
            scrollY_ = std::clamp(b.y - 8, 0, maxScroll());
            return;
        }
    }
}

// Collect named form controls and build an application/x-www-form-urlencoded
// query string (GET semantics).
static std::string buildFormQuery(const std::shared_ptr<Node>& form,
                                  const std::shared_ptr<Node>& submitter) {
    std::string qs;
    std::function<void(const std::shared_ptr<Node>&)> walk =
        [&](const std::shared_ptr<Node>& n) {
            for (auto& c : n->children) {
                std::string name = c->attrs.count("name") ? c->attrs["name"] : "";
                std::string type = c->attrs.count("type")
                                     ? c->attrs["type"] : (c->tag == "input" ? "text" : "");
                if (!name.empty()) {
                    if (c->tag == "input") {
                        if (type == "checkbox" || type == "radio") {
                            if (c->attrs.count("checked")) {
                                std::string v = c->attrs.count("value")
                                                  ? c->attrs["value"] : "on";
                                if (!qs.empty()) qs += "&";
                                qs += urlEncode(name) + "=" + urlEncode(v);
                            }
                        } else if (type != "submit" && type != "button" &&
                                   type != "file") {
                            std::string v = c->attrs.count("value")
                                              ? c->attrs["value"] : "";
                            if (!qs.empty()) qs += "&";
                            qs += urlEncode(name) + "=" + urlEncode(v);
                        }
                        if (c == submitter && type == "submit") {
                            std::string v = c->attrs.count("value")
                                              ? c->attrs["value"] : "Submit";
                            if (!qs.empty()) qs += "&";
                            qs += urlEncode(name) + "=" + urlEncode(v);
                        }
                    } else if (c->tag == "textarea") {
                        std::string v;
                        for (auto& tc : c->children)
                            if (tc->tag == "text") v += tc->text;
                        if (!qs.empty()) qs += "&";
                        qs += urlEncode(name) + "=" + urlEncode(v);
                    } else if (c->tag == "select") {
                        std::string v;
                        for (auto& oc : c->children) {
                            if (oc->tag == "option" && oc->attrs.count("selected")) {
                                for (auto& tc : oc->children)
                                    if (tc->tag == "text") v += tc->text;
                            }
                        }
                        if (v.empty()) {
                            for (auto& oc : c->children) {
                                if (oc->tag == "option") {
                                    for (auto& tc : oc->children)
                                        if (tc->tag == "text") v += tc->text;
                                    break;
                                }
                            }
                        }
                        if (!qs.empty()) qs += "&";
                        qs += urlEncode(name) + "=" + urlEncode(v);
                    }
                }
                walk(c);
            }
        };
    walk(form);
    return qs;
}

void Browser::fireSubmit_(std::shared_ptr<Node> form) {
    if (!form) return;
    std::string onsubmit = findAttr(form, "onsubmit");
    if (!onsubmit.empty()) {
        std::cout << "[Running onsubmit] " << onsubmit << "\n";
        js_.executeEvent(onsubmit, "submit", form);
        // v2.15: DO NOT return here. The original code returned after
        // running onsubmit, which meant forms with an onsubmit handler
        // NEVER actually submitted — the JS ran (e.g. setting a hidden
        // field's value) but submitForm_ was never called, so the
        // browser never navigated. This was the root cause of the
        // "Enter key and Search button don't initiate search" bug:
        // the YouTube search form (v2.14) and the DDG search form both
        // have onsubmit handlers, so they ran the JS but never
        // submitted. The standard browser behaviour is: onsubmit's
        // return value controls whether to proceed (return false =
        // cancel), but our JS engine's executeEvent doesn't surface
        // the return value, so we always proceed. This matches the
        // behaviour for forms WITHOUT onsubmit (which always submit).
    }
    js_.dispatchEvent("submit", form);
    submitForm_(form);
}

bool Browser::submitForm_(std::shared_ptr<Node> form) {
    if (!form) return false;
    std::string action = form->attrs.count("action") ? form->attrs["action"] : "";
    std::string method = form->attrs.count("method")
                           ? form->attrs["method"] : "GET";
    std::string qs = buildFormQuery(form, nullptr);
    std::string target = action.empty()
        ? (documentUrl_.empty() ? currentPath_ : documentUrl_)
        : resolveLink_(action);
    if (method == "GET" && !qs.empty()) {
        target += (target.find('?') == std::string::npos ? "?" : "&") + qs;
    }
    navigate(target);
    return true;
}

// ---------------------------------------------------------------------------
// Event handling: click
// ---------------------------------------------------------------------------

void Browser::updateHoverCursor() {
    // linkAt expects content-relative coordinates (the layout's
    // link hit-rects use document-space y, which we then offset by
    // scrollY_). The mouse position from SDL is in window coords, so
    // we have to subtract contentY() before passing it in.
    std::string h;
    std::shared_ptr<Node> newHovered;
    if (mouseY_ >= contentY() && mouseY_ < contentY() + contentH()) {
        int cx = mouseX_;
        int cy = mouseY_ - contentY();
        int my = cy + scrollY_;
        // Find link hovered (for cursor + hoveredHref).
        for (auto& lk : layout_.links) {
            if (cx >= lk.x && cx <= lk.x + lk.w &&
                my >= lk.y && my <= lk.y + lk.h) {
                h = lk.href;
                newHovered = lk.sourceNode.lock();
                break;
            }
        }
        // If not on a link, find the topmost box under the cursor so
        // CSS `:hover` on non-link elements (e.g. div:hover) works.
        if (!newHovered) {
            for (auto it = layout_.boxes.rbegin(); it != layout_.boxes.rend(); ++it) {
                const Box& b = *it;
                if (cx >= b.x && cx <= b.x + b.w &&
                    my >= b.y && my <= b.y + b.h) {
                    newHovered = b.sourceNode.lock();
                    if (newHovered) break;
                }
            }
        }
    }

    if (h != hoveredHref_) {
        hoveredHref_ = h;
        if (h.empty()) {
            if (usingHandCursor_) { SDL_SetCursor(cursorArrow_); usingHandCursor_ = false; }
        } else {
            if (!usingHandCursor_) { SDL_SetCursor(cursorHand_); usingHandCursor_ = true; }
        }
    }
    // If hovered node changed, relayout so :hover styles apply.
    if (newHovered != hoveredNode_) {
        hoveredNode_ = newHovered;
        needsRelayout_ = true;
    }
}

// ---------------------------------------------------------------------------
// Mouse text selection (round 3)
// ---------------------------------------------------------------------------

void Browser::startSelection_(int contentX, int contentY) {
    selAnchor_ = hitTestText(layout_, font_, contentX,
                             contentY + scrollY_);
    selFocus_ = selAnchor_;
    selecting_ = true;
    hasSelection_ = false;
}

void Browser::updateSelection_(int contentX, int contentY) {
    selFocus_ = hitTestText(layout_, font_, contentX, contentY + scrollY_);
    hasSelection_ = selAnchor_.valid && selFocus_.valid &&
                    !(selAnchor_ == selFocus_);
}

void Browser::finishSelection_() {
    selecting_ = false;
    hasSelection_ = selAnchor_.valid && selFocus_.valid &&
                    !(selAnchor_ == selFocus_);
}

void Browser::clearSelection_() {
    selecting_ = false;
    hasSelection_ = false;
    selAnchor_ = selFocus_ = TextPos{};
}

// Double-click: select the word under the cursor. Words are maximal
// runs of non-whitespace in the line's concatenated text.
void Browser::selectWordAt_(int contentX, int contentY) {
    TextPos p = hitTestText(layout_, font_, contentX, contentY + scrollY_);
    if (!p.valid) return;
    const Box* b = nullptr;
    if (p.box >= 0 && p.box < (int)layout_.boxes.size())
        b = &layout_.boxes[p.box];
    if (!b || p.line < 0 || p.line >= (int)b->lines.size()) return;

    std::string line;
    for (auto& r : b->lines[p.line])
        if (!r.isImage) line += r.text;
    if (line.empty()) return;

    auto isWordChar = [](unsigned char c) { return std::isalnum(c) || c == '_'; };
    int lo = p.byte, hi = p.byte;
    while (lo > 0 && isWordChar((unsigned char)line[lo - 1])) --lo;
    while (hi < (int)line.size() && isWordChar((unsigned char)line[hi])) ++hi;
    if (lo == hi) {  // clicked whitespace — select the single codepoint
        lo = p.byte;
        hi = (p.byte + 1 <= (int)line.size()) ? p.byte + 1 : p.byte;
    }
    selAnchor_ = p; selAnchor_.byte = lo;
    selFocus_  = p; selFocus_.byte  = hi;
    selecting_ = true;   // drag extends the word selection
    hasSelection_ = true;
}

std::vector<SelectionSpan> Browser::selectionSpans_() const {
    std::vector<SelectionSpan> spans;
    if (!hasSelection_ || !selAnchor_.valid || !selFocus_.valid)
        return spans;
    TextPos s = selAnchor_, e = selFocus_;
    if (e < s) std::swap(s, e);

    int nBoxes = (int)layout_.boxes.size();
    if (s.box < 0 || s.box >= nBoxes || e.box < 0 || e.box >= nBoxes)
        return spans;

    for (int bi = s.box; bi <= e.box; ++bi) {
        const Box& b = layout_.boxes[bi];
        if (b.isImage || b.lines.empty()) continue;
        int liLo = (bi == s.box) ? s.line : 0;
        int liHi = (bi == e.box) ? e.line : (int)b.lines.size() - 1;
        for (int li = liLo; li <= liHi && li < (int)b.lines.size(); ++li) {
            SelectionSpan sp;
            sp.boxIdx = bi;
            sp.lineIdx = li;
            sp.startByte = (bi == s.box && li == s.line) ? s.byte : 0;
            sp.endByte = (bi == e.box && li == e.line)
                             ? e.byte : lineByteLength(b, li);
            if (sp.endByte > sp.startByte) spans.push_back(sp);
        }
    }
    return spans;
}

void Browser::copySelection_() const {
    if (!hasSelection_) return;
    std::string text = textBetween(layout_, selAnchor_, selFocus_);
    if (text.empty()) return;
    SDL_SetClipboardText(text.c_str());
    std::cout << "[clipboard] copied " << text.size() << " bytes\n";
}

// A press is "actionable" when the old mousedown-activates flow should
// own it: link rects, form widgets, buttons, anchors, onclick handlers.
bool Browser::isActionableHit_(int contentX, int contentY) const {
    int mx = contentX, my = contentY + scrollY_;
    for (auto& lk : layout_.links) {
        if (mx >= lk.x && mx <= lk.x + lk.w &&
            my >= lk.y && my <= lk.y + lk.h) return true;
    }
    for (auto it = layout_.boxes.rbegin(); it != layout_.boxes.rend(); ++it) {
        const Box& b = *it;
        if (mx < b.x || mx > b.x + b.w || my < b.y || my > b.y + b.h) continue;
        auto n = b.sourceNode.lock();
        if (!n) continue;
        if (n->tag == "input" || n->tag == "button" || n->tag == "select" ||
            n->tag == "textarea" || n->tag == "a" || n->tag == "video" ||
            n->tag == "audio" ||
            n->attrs.count("onclick") || n->attrs.count("onmousedown"))
            return true;
    }
    return false;
}

void Browser::handleClick(int sx, int sy) {
    // Translate to content coords.
    int mx = sx;
    int my = sy + scrollY_;

    // Image viewer overlay consumes every click while open. NOTE:
    // handleClick receives CONTENT coords while viewerCloseRect_ is in
    // window coords (it is drawn by drawImageViewer_ on the window) —
    // convert back before hit-testing.
    if (viewerOpen_) {
        int wx = sx, wy = sy + contentY();
        if (hitButton_(viewerCloseRect_.x, viewerCloseRect_.y,
                       viewerCloseRect_.w, viewerCloseRect_.h, wx, wy))
            closeImageViewer_();
        return;
    }

    // Media boxes (<video>/<audio>): their controls take priority over
    // anything underneath — a video widget is interactive surface, not
    // passive document area.
    for (auto it = layout_.boxes.rbegin(); it != layout_.boxes.rend(); ++it) {
        const Box& b = *it;
        if (!b.isVideo && !b.isAudio) continue;
        if (mx < b.x || mx >= b.x + b.w || my < b.y || my >= b.y + b.h)
            continue;
        auto p = b.mediaPath.empty() ? nullptr : media::findPlayer(b.mediaPath);
        if (!p) return;   // still loading: swallow the click
        MediaHit h = mediaHitTest(b, mx, my);
        switch (h.part) {
            case MediaHit::Play:
                p->toggle();
                break;
            case MediaHit::Mute:
                p->setMuted(!p->muted());
                break;
            case MediaHit::Volume:
                p->setVolume((float)h.frac);
                p->setMuted(false);
                break;
            case MediaHit::Seek:
                p->seek(h.frac * p->duration());
                // Remember the box so holding the button scrubs (drag).
                seekDragBox_ = (int)layout_.boxes.size() - 1 -
                               (int)(it - layout_.boxes.rbegin());
                break;
            case MediaHit::Body:
                if (b.isVideo) p->toggle();   // Chrome: click video toggles
                break;
            default:
                break;   // bar background: no action
        }
        frameDirty_ = true;
        return;
    }

    // First, see if it's a link.
    for (auto& lk : layout_.links) {
        if (mx >= lk.x && mx <= lk.x + lk.w &&
            my >= lk.y && my <= lk.y + lk.h) {
            std::cout << "[Link clicked] " << lk.href << "\n";

            std::shared_ptr<Node> aNode = lk.sourceNode.lock();
            if (!aNode) {
                std::function<std::shared_ptr<Node>(const std::shared_ptr<Node>&)>
                findA = [&](const std::shared_ptr<Node>& n) -> std::shared_ptr<Node> {
                    if (n->tag == "a" && n->attrs.count("href")
                        && n->attrs["href"] == lk.href) return n;
                    for (auto& c : n->children) if (auto r = findA(c)) return r;
                    return nullptr;
                };
                aNode = findA(dom_);
            }
            if (aNode) {
                std::string onclick = findAttr(aNode, "onclick");
                if (!onclick.empty()) {
                    std::cout << "[Running onclick] " << onclick << "\n";
                    js_.executeEvent(onclick, "click", aNode);
                }
                // Also fire any addEventListener("click", ...) handlers.
                js_.dispatchEvent("click", aNode);
            }

            // javascript: URLs run the code as the "navigation".
            if (lk.href.compare(0, 11, "javascript:") == 0) {
                js_.execute(lk.href.substr(11));
                return;
            }
            // Pure fragment links scroll to the anchor without reloading.
            if (!lk.href.empty() && lk.href[0] == '#') {
                scrollToFragment_(lk.href.substr(1));
                return;
            }
            std::string resolved = resolveLink_(lk.href);
            navigate(resolved);
            return;
        }
    }

    // Plain images: hit-test the click against every laid-out <img>
    // fragment rect. rectForNode covers BOTH atomic image boxes and
    // inline image runs inside text boxes (a bare <img> child of a
    // container flows as an inline run and has no box of its own).
    // Links were handled above, so link-wrapped images never get here;
    // onclick/onmousedown on the element or an ancestor keeps the normal
    // dispatch path.
    {
        std::string viewerSrc;
        std::function<bool(const std::shared_ptr<Node>&)> walkImg =
            [&](const std::shared_ptr<Node>& n) -> bool {
                for (auto& c : n->children) {
                    if (c->tag == "img" &&
                        findAttr(c, "onclick").empty() &&
                        findAttr(c, "onmousedown").empty()) {
                        DOMRect r;
                        if (rectForNode(layout_, font_, c, r) &&
                            mx >= r.x && mx <= r.right() &&
                            my >= r.y && my <= r.bottom()) {
                            std::string src = pickImageSrc(c);
                            if (!src.empty()) {
                                viewerSrc =
                                    ResourceLoader::instance().resolve(src);
                                return true;
                            }
                        }
                    }
                    if (walkImg(c)) return true;
                }
                return false;
            };
        if (dom_ && walkImg(dom_) && !viewerSrc.empty()) {
            openImageViewer_(viewerSrc);
            return;
        }
    }

    // Not a link — find any node whose box covers the click.
    //
    // Iterate in REVERSE: topmost (last-painted, most-specific) box
    // first. We only stop when a widget actually consumed the click, so
    // a transparent container with no handler falls through to the
    // content beneath it.
    for (auto it = layout_.boxes.rbegin(); it != layout_.boxes.rend(); ++it) {
        const Box& b = *it;
        if (mx >= b.x && mx <= b.x + b.w &&
            my >= b.y && my <= b.y + b.h) {
            std::shared_ptr<Node> hitNode = b.sourceNode.lock();
            if (!hitNode) continue;

            // <input> handling. NOTE: every <input> type must be dealt
            // with inside THIS block — the old code had a second
            // `if (hitNode->tag == "input")` further down for
            // checkbox/radio, which was UNREACHABLE because this block
            // returned for all inputs first. Toggling a checkbox never
            // worked because of that.
            if (hitNode->tag == "input") {
                std::string type = hitNode->attrs.count("type")
                                     ? hitNode->attrs["type"] : "text";
                if (type == "checkbox") {
                    if (hitNode->attrs.count("checked"))
                        hitNode->attrs.erase("checked");
                    else
                        hitNode->attrs["checked"] = "checked";
                    js_.dispatchEvent("change", hitNode);
                    needsRelayout_ = true;
                    return;
                }
                if (type == "radio") {
                    // Uncheck all radios with the same name in the same
                    // form, then check this one.
                    std::string name = hitNode->attrs.count("name")
                                         ? hitNode->attrs["name"] : "";
                    auto form = hitNode->parent.lock();
                    while (form && form->tag != "form") form = form->parent.lock();
                    if (form) {
                        std::function<void(const std::shared_ptr<Node>&)> walk =
                            [&](const std::shared_ptr<Node>& n) {
                                for (auto& c : n->children) {
                                    if (c->tag == "input" &&
                                        c->attrs.count("type") &&
                                        c->attrs["type"] == "radio" &&
                                        c->attrs.count("name") &&
                                        c->attrs["name"] == name) {
                                        c->attrs.erase("checked");
                                    }
                                    walk(c);
                                }
                            };
                        walk(form);
                    }
                    hitNode->attrs["checked"] = "checked";
                    js_.dispatchEvent("change", hitNode);
                    needsRelayout_ = true;
                    return;
                }
                if (type == "submit" || type == "button") {
                    // Clicking submit fires onclick (if any), the
                    // listeners, then submits the surrounding form.
                    std::string onclick = findAttr(hitNode, "onclick");
                    if (!onclick.empty())
                        js_.executeEvent(onclick, "click", hitNode);
                    js_.dispatchEvent("click", hitNode);
                    auto form = hitNode->parent.lock();
                    while (form && form->tag != "form") form = form->parent.lock();
                    if (form) fireSubmit_(form);
                    return;
                }
                // Plain text-ish input: focus it (caret at end unless
                // already focused).
                if (focusedNode_ != hitNode) {
                    focusedNode_ = hitNode;
                    carets_[hitNode.get()] = (int)(hitNode->attrs.count("value")
                                                     ? hitNode->attrs["value"].size() : 0);
                    SDL_StartTextInput();
                }
                needsRelayout_ = true;
                return;
            }

            if (hitNode->tag == "textarea") {
                if (focusedNode_ != hitNode) {
                    focusedNode_ = hitNode;
                    std::string v;
                    for (auto& c : hitNode->children)
                        if (c->tag == "text") v += c->text;
                    carets_[hitNode.get()] = (int)v.size();
                    SDL_StartTextInput();
                }
                needsRelayout_ = true;
                return;
            }

            // <select> click: cycle to the next <option>.
            if (hitNode->tag == "select") {
                std::vector<std::shared_ptr<Node>> opts;
                for (auto& c : hitNode->children)
                    if (c->tag == "option") opts.push_back(c);
                if (opts.size() > 1) {
                    size_t cur = 0;
                    for (size_t i = 0; i < opts.size(); ++i) {
                        if (opts[i]->attrs.count("selected")) { cur = i; break; }
                    }
                    opts[cur]->attrs.erase("selected");
                    size_t next = (cur + 1) % opts.size();
                    opts[next]->attrs["selected"] = "selected";
                    js_.dispatchEvent("change", hitNode);
                    needsRelayout_ = true;
                }
                return;
            }

            // Buttons (<button>) and other clickable elements.
            std::string onclick = findAttr(hitNode, "onclick");
            bool fired = false;
            if (!onclick.empty()) {
                std::cout << "[Running onclick] " << onclick << "\n";
                js_.executeEvent(onclick, "click", hitNode);
                fired = true;
            }
            if (js_.dispatchEvent("click", hitNode)) fired = true;
            if (fired) return;
            // Plain image with no handler: open the integrated image
            // viewer (lightbox with zoom/pan). Images inside links are
            // handled by the link branch above and never get here.
            if (hitNode->tag == "img" && b.isImage && !b.imagePath.empty()) {
                openImageViewer_(b.imagePath);
                return;
            }
            // Not clickable — keep looking underneath.
        }
    }
}

// ---------------------------------------------------------------------------
// Event handling: keyboard / mouse / window
// ---------------------------------------------------------------------------

namespace {
// Insert UTF-8 text into `value` at byte offset `caret`, advancing caret.
static void insertAtCaret(std::string& value, int& caret, const std::string& txt) {
    caret = std::clamp(caret, 0, (int)value.size());
    value.insert((size_t)caret, txt);
    caret += (int)txt.size();
}
} // namespace

bool Browser::handleEvent(const SDL_Event& e) {
    // Any event may change what's on screen (input, hover, expose,
    // resize...). Cheap: one repaint per drained event batch, and none
    // at all while the loop is idle.
    frameDirty_ = true;
    switch (e.type) {
        case SDL_QUIT:
            return false;

        case SDL_DROPFILE: {
            // Drag & drop a file onto the window = open it.
            if (e.drop.file) {
                std::string file = e.drop.file;
                SDL_free(e.drop.file);
                std::cout << "[drop] " << file << "\n";
                navigate(file);
            }
            break;
        }

        case SDL_KEYDOWN: {
            int sym = e.key.keysym.sym;
            Uint16 mod = e.key.keysym.mod;

            // Alert dialog swallows keys: Enter/Esc close it.
            if (alertVisible_) {
                if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER ||
                    sym == SDLK_ESCAPE) {
                    alertVisible_ = false;
                }
                return true;
            }

            // Image viewer is modal: it eats every key (Esc/Enter close,
            // +/-/0 zoom, arrows pan).
            if (viewerOpen_) {
                viewerKey_(sym, mod);
                return true;
            }

            // If a form input is focused, route editing keys to it. The
            // address bar takes precedence (clicking it clears focus).
            if (focusedNode_ && !addressEditing_ && !finding_) {
                if (focusedNode_->tag == "input") {
                    std::string v = focusedNode_->attrs.count("value")
                                      ? focusedNode_->attrs["value"] : "";
                    int caret = focusedCaret();
                    caret = std::clamp(caret, 0, (int)v.size());

                    if (sym == SDLK_BACKSPACE) {
                        if (caret > 0) {
                            int prev = utf8Prev(v, caret);
                            v.erase((size_t)prev, (size_t)(caret - prev));
                            caret = prev;
                        }
                    } else if (sym == SDLK_DELETE) {
                        if (caret < (int)v.size()) {
                            int next = utf8Next(v, caret);
                            v.erase((size_t)caret, (size_t)(next - caret));
                        }
                    } else if (sym == SDLK_LEFT) {
                        caret = utf8Prev(v, caret);
                        setFocusedCaret(caret);
                        return true;
                    } else if (sym == SDLK_RIGHT) {
                        caret = utf8Next(v, caret);
                        setFocusedCaret(caret);
                        return true;
                    } else if (sym == SDLK_HOME) {
                        setFocusedCaret(0);
                        return true;
                    } else if (sym == SDLK_END) {
                        setFocusedCaret((int)v.size());
                        return true;
                    } else if ((mod & KMOD_CTRL) && sym == SDLK_v) {
                        pasteClipboard_(v, caret);
                    } else if ((mod & KMOD_CTRL) && sym == SDLK_c) {
                        SDL_SetClipboardText(v.c_str());
                        return true;
                    } else if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) {
                        // Enter in <input> = submit the form (best effort):
                        // fire onsubmit / listeners, then GET-navigate.
                        auto f = focusedNode_->parent.lock();
                        while (f && f->tag != "form") f = f->parent.lock();
                        setFocusedCaret(caret);
                        focusedNode_->attrs["value"] = v;
                        if (f) fireSubmit_(f);
                        focusedNode_.reset();
                        SDL_StopTextInput();
                        return true;
                    } else if (sym == SDLK_TAB) {
                        focusedNode_->attrs["value"] = v;
                        advanceFocus_(mod & KMOD_SHIFT);
                        return true;
                    } else {
                        setFocusedCaret(caret);
                        focusedNode_->attrs["value"] = v;
                        return true;   // consume other keys while typing
                    }
                    focusedNode_->attrs["value"] = v;
                    setFocusedCaret(caret);
                    needsRelayout_ = true;
                    return true;
                }

                if (focusedNode_->tag == "textarea") {
                    std::shared_ptr<Node> textChild;
                    for (auto& c : focusedNode_->children) {
                        if (c->tag == "text") { textChild = c; break; }
                    }
                    std::string v = textChild ? textChild->text : "";
                    int caret = focusedCaret();
                    caret = std::clamp(caret, 0, (int)v.size());

                    if (sym == SDLK_BACKSPACE) {
                        if (caret > 0) {
                            int prev = utf8Prev(v, caret);
                            v.erase((size_t)prev, (size_t)(caret - prev));
                            caret = prev;
                        }
                    } else if (sym == SDLK_DELETE) {
                        if (caret < (int)v.size()) {
                            int next = utf8Next(v, caret);
                            v.erase((size_t)caret, (size_t)(next - caret));
                        }
                    } else if (sym == SDLK_LEFT) {
                        caret = utf8Prev(v, caret);
                        setFocusedCaret(caret);
                        return true;
                    } else if (sym == SDLK_RIGHT) {
                        caret = utf8Next(v, caret);
                        setFocusedCaret(caret);
                        return true;
                    } else if (sym == SDLK_UP || sym == SDLK_HOME) {
                        // Simple: jump to start of line or start.
                        setFocusedCaret(0);
                        return true;
                    } else if (sym == SDLK_DOWN || sym == SDLK_END) {
                        setFocusedCaret((int)v.size());
                        return true;
                    } else if ((mod & KMOD_CTRL) && sym == SDLK_v) {
                        pasteClipboard_(v, caret);
                    } else if ((mod & KMOD_CTRL) && sym == SDLK_c) {
                        SDL_SetClipboardText(v.c_str());
                        return true;
                    } else if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) {
                        insertAtCaret(v, caret, "\n");
                    } else if (sym == SDLK_TAB) {
                        advanceFocus_(mod & KMOD_SHIFT);
                        return true;
                    } else {
                        setFocusedCaret(caret);
                        return true;
                    }
                    if (!textChild) {
                        textChild = std::make_shared<Node>();
                        textChild->tag = "text";
                        textChild->parent = focusedNode_;
                        focusedNode_->children.push_back(textChild);
                    }
                    textChild->text = v;
                    setFocusedCaret(caret);
                    needsRelayout_ = true;
                    return true;
                }

                // Non-text focusables (links, buttons, select): Tab moves
                // on, Enter/Space activates buttons and selects.
                if (sym == SDLK_TAB) {
                    advanceFocus_(mod & KMOD_SHIFT);
                    return true;
                }
                if ((sym == SDLK_RETURN || sym == SDLK_KP_ENTER ||
                     sym == SDLK_SPACE) &&
                    (focusedNode_->tag == "button" ||
                     focusedNode_->tag == "select")) {
                    handleClickForNode_(focusedNode_);
                    return true;
                }
                return true;   // swallow other keys
            }

            // If we're editing the address bar, ALL keystrokes go there.
            if (addressEditing_) {
                if (sym == SDLK_ESCAPE) {
                    addressEditing_ = false;
                    addressInput_.clear();
                    SDL_StopTextInput();
                } else if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) {
                    if (!addressInput_.empty()) {
                        navigate(addressInput_);
                    }
                    addressEditing_ = false;
                    addressInput_.clear();
                    SDL_StopTextInput();
                } else if (sym == SDLK_BACKSPACE) {
                    if (addressCaret_ > 0) {
                        int prev = utf8Prev(addressInput_, addressCaret_);
                        addressInput_.erase((size_t)prev,
                                            (size_t)(addressCaret_ - prev));
                        addressCaret_ = prev;
                    }
                } else if (sym == SDLK_DELETE) {
                    if (addressCaret_ < (int)addressInput_.size()) {
                        int next = utf8Next(addressInput_, addressCaret_);
                        addressInput_.erase((size_t)addressCaret_,
                                            (size_t)(next - addressCaret_));
                    }
                } else if (sym == SDLK_LEFT) {
                    addressCaret_ = utf8Prev(addressInput_, addressCaret_);
                } else if (sym == SDLK_RIGHT) {
                    addressCaret_ = utf8Next(addressInput_, addressCaret_);
                } else if (sym == SDLK_HOME) {
                    addressCaret_ = 0;
                } else if (sym == SDLK_END) {
                    addressCaret_ = (int)addressInput_.size();
                } else if ((mod & KMOD_CTRL) && sym == SDLK_v) {
                    pasteClipboard_(addressInput_, addressCaret_);
                } else if ((mod & KMOD_CTRL) && sym == SDLK_a) {
                    addressCaret_ = 0;
                } else {
                    // Printable text arrives via SDL_TEXTINPUT.
                }
                return true;
            }

            // Find mode keystrokes.
            if (finding_) {
                if (sym == SDLK_ESCAPE) {
                    finding_ = false;
                    findQuery_.clear();
                    findMatches_.clear();
                } else if (sym == SDLK_RETURN) {
                    findNext_((mod & KMOD_SHIFT) ? -1 : 1);
                } else if (sym == SDLK_BACKSPACE) {
                    if (!findQuery_.empty()) findQuery_.pop_back();
                    runFind_();
                }
                return true;
            }

            if (sym == SDLK_ESCAPE) return false;
            // Ctrl+C with an active text selection: copy it (form fields
            // and bars copy themselves in their own branches above).
            if ((mod & KMOD_CTRL) && sym == SDLK_c) {
                if (hasSelection_) {
                    copySelection_();
                    return true;
                }
            }
            if (sym == SDLK_DOWN)  scrollY_ = std::min(maxScroll(), scrollY_ + 40);
            if (sym == SDLK_UP)    scrollY_ = std::max(0, scrollY_ - 40);
            if (sym == SDLK_PAGEDOWN) scrollY_ = std::min(maxScroll(), scrollY_ + contentH());
            if (sym == SDLK_PAGEUP)   scrollY_ = std::max(0, scrollY_ - contentH());
            if (sym == SDLK_HOME)  scrollY_ = 0;
            if (sym == SDLK_END)   scrollY_ = maxScroll();

            // Alt+Left / Alt+Right = back / forward
            if ((mod & KMOD_ALT) && sym == SDLK_LEFT)  { back();     return true; }
            if ((mod & KMOD_ALT) && sym == SDLK_RIGHT) { forward();  return true; }
            // v = play the current page in mpv (YouTube watch pages and
            // direct media URLs). No-op with a status hint elsewhere.
            if (sym == SDLK_v && !(mod & (KMOD_CTRL | KMOD_ALT | KMOD_GUI)) &&
                !addressEditing_ && !finding_) {
                maybePlayInMpv_(currentPath_);
                return true;
            }
            // F5 or Ctrl+R = reload
            if (sym == SDLK_F5 || ((mod & KMOD_CTRL) && sym == SDLK_r)) {
                reload();
                return true;
            }
            // Ctrl+F = find in page
            if ((mod & KMOD_CTRL) && sym == SDLK_f) {
                finding_ = true;
                findQuery_.clear();
                findMatches_.clear();
                SDL_StartTextInput();
                return true;
            }
            // Ctrl+= / Ctrl+Plus = zoom in; Ctrl+- zoom out; Ctrl+0 reset.
            if ((mod & KMOD_CTRL) && (sym == SDLK_EQUALS || sym == SDLK_PLUS ||
                                       sym == SDLK_KP_PLUS)) {
                zoom_ = std::min(3.0f, zoom_ + 0.1f);
                applyZoom_();
                return true;
            }
            if ((mod & KMOD_CTRL) && (sym == SDLK_MINUS || sym == SDLK_KP_MINUS)) {
                zoom_ = std::max(0.5f, zoom_ - 0.1f);
                applyZoom_();
                return true;
            }
            if ((mod & KMOD_CTRL) && sym == SDLK_0) {
                zoom_ = 1.0f;
                applyZoom_();
                return true;
            }
            // Ctrl+L = focus the address bar to type a URL.
            if ((mod & KMOD_CTRL) && sym == SDLK_l) {
                addressEditing_ = true;
                addressInput_ = currentPath_;
                addressCaret_ = (int)addressInput_.size();
                SDL_StartTextInput();
                return true;
            }
            // Ctrl+U = view source of the current page.
            if ((mod & KMOD_CTRL) && sym == SDLK_u) {
                navigate("view-source:" + currentPath_);
                return true;
            }
            // Ctrl+S = save the current page's HTML next to the browser.
            if ((mod & KMOD_CTRL) && sym == SDLK_s) {
                savePage_();
                return true;
            }
            // Ctrl+D = bookmark / unbookmark the current page.
            if ((mod & KMOD_CTRL) && sym == SDLK_d) {
                toggleBookmark_();
                return true;
            }
            // Ctrl+B = toggle the bookmarks bar.
            if ((mod & KMOD_CTRL) && sym == SDLK_b) {
                bookmarksVisible_ = !bookmarksVisible_;
                relayout_();
                return true;
            }
            // F11 = fullscreen toggle.
            if (sym == SDLK_F11) {
                Uint32 flags = SDL_GetWindowFlags(win_);
                bool fs = (flags & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
                SDL_SetWindowFullscreen(win_, fs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
                return true;
            }
            updateHoverCursor();
            break;
        }

        case SDL_TEXTINPUT: {
            // SDL sends TEXTINPUT for printable characters; we route them
            // to whichever input is active (focused form input > address
            // bar > find mode).
            std::string txt = e.text.text;
            if (addressEditing_) {
                insertAtCaret(addressInput_, addressCaret_, txt);
                return true;
            }
            if (finding_) {
                findQuery_ += txt;
                runFind_();
                return true;
            }
            if (focusedNode_) {
                if (focusedNode_->tag == "input") {
                    std::string v = focusedNode_->attrs.count("value")
                                      ? focusedNode_->attrs["value"] : "";
                    int caret = focusedCaret();
                    insertAtCaret(v, caret, txt);
                    focusedNode_->attrs["value"] = v;
                    setFocusedCaret(caret);
                    needsRelayout_ = true;
                } else if (focusedNode_->tag == "textarea") {
                    std::shared_ptr<Node> textChild;
                    for (auto& c : focusedNode_->children) {
                        if (c->tag == "text") { textChild = c; break; }
                    }
                    if (!textChild) {
                        textChild = std::make_shared<Node>();
                        textChild->tag = "text";
                        textChild->parent = focusedNode_;
                        focusedNode_->children.push_back(textChild);
                    }
                    std::string v = textChild->text;
                    int caret = focusedCaret();
                    insertAtCaret(v, caret, txt);
                    textChild->text = v;
                    setFocusedCaret(caret);
                    needsRelayout_ = true;
                }
                return true;
            }
            break;
        }

        case SDL_MOUSEWHEEL:
            // Viewer open: wheel zooms instead of scrolling the page.
            if (viewerOpen_) {
                viewerZoom_ = e.wheel.y > 0
                    ? std::min(12.0f, viewerZoom_ * 1.15f)
                    : std::max(0.05f, viewerZoom_ / 1.15f);
                break;
            }
            scrollY_ = std::clamp(scrollY_ - e.wheel.y * 40, 0, maxScroll());
            updateHoverCursor();
            break;

        case SDL_MOUSEMOTION:
            mouseX_ = e.motion.x;
            mouseY_ = e.motion.y;
            // Image viewer pan drag.
            if (viewerOpen_) {
                if (viewerPanning_) {
                    viewerPanX_ += e.motion.xrel;
                    viewerPanY_ += e.motion.yrel;
                }
                break;
            }
            // Media seek-bar drag: scrub while the button is held.
            if (seekDragBox_ >= 0 &&
                seekDragBox_ < (int)layout_.boxes.size()) {
                const Box& b = layout_.boxes[seekDragBox_];
                int my = mouseY_ - contentY() + scrollY_;
                MediaHit h = mediaHitTest(b, mouseX_, my);
                if (h.part == MediaHit::Seek) {
                    auto p = media::findPlayer(b.mediaPath);
                    if (p && p->duration() > 0) {
                        p->seek(h.frac * p->duration());
                        frameDirty_ = true;
                    }
                }
                break;
            }
            // Scrollbar drag: update scroll based on thumb position.
            if (draggingScrollbar_) {
                int total = layout_.contentHeight;
                int visible = contentH();
                if (total > visible) {
                    int trackH = contentH();
                    int thumbH = std::max(20, trackH * visible / total);
                    int trackY = contentY();
                    int rel = (mouseY_ - dragThumbOffset_) - trackY;
                    int maxRel = trackH - thumbH;
                    if (maxRel > 0) {
                        scrollY_ = std::clamp(rel * (total - visible) / maxRel,
                                              0, total - visible);
                    }
                }
                break;
            }
            // Click in chrome area should not toggle link cursor.
            if (mouseY_ < contentY() || mouseY_ > contentY() + contentH()) {
                if (usingHandCursor_) { SDL_SetCursor(cursorArrow_); usingHandCursor_ = false; }
            } else {
                updateHoverCursor();
            }
            // Live selection drag: track the focus position.
            if (selecting_) {
                updateSelection_(mouseX_, mouseY_ - contentY());
            }
            break;

        case SDL_MOUSEBUTTONDOWN:
            if (e.button.button == SDL_BUTTON_LEFT) {
                int mx = e.button.x;
                int my = e.button.y;

                // Image viewer: close button or start a pan drag; the
                // overlay covers the page, so nothing under it reacts.
                if (viewerOpen_) {
                    if (hitButton_(viewerCloseRect_.x, viewerCloseRect_.y,
                                   viewerCloseRect_.w, viewerCloseRect_.h,
                                   mx, my)) {
                        closeImageViewer_();
                    } else {
                        viewerPanning_ = true;
                        viewerGrabX_ = mx;
                        viewerGrabY_ = my;
                    }
                    return true;
                }

                // Alert modal eats clicks: only the OK button matters.
                if (alertVisible_) {
                    if (hitButton_(alertOkRect_.x, alertOkRect_.y,
                                   alertOkRect_.w, alertOkRect_.h, mx, my)) {
                        alertVisible_ = false;
                    }
                    return true;
                }

                // Scrollbar hit-test: if the click is on the thumb, start
                // a drag; if it's on the track, jump-scroll.
                int tx, ty, tw, th;
                scrollbarThumbRect_(tx, ty, tw, th);
                int trackX = winW_ - SCROLLBAR_W;
                int trackY = contentY();
                int trackH = contentH();
                if (mx >= trackX && mx < trackX + SCROLLBAR_W &&
                    my >= trackY && my < trackY + trackH) {
                    if (my >= ty && my < ty + th) {
                        draggingScrollbar_ = true;
                        dragThumbOffset_ = my - ty;
                    } else {
                        if (my < ty) scrollY_ = std::max(0, scrollY_ - contentH());
                        else         scrollY_ = std::min(maxScroll(), scrollY_ + contentH());
                    }
                    return true;
                }

                // Address bar back / forward / reload / bookmark buttons.
                int bx = 4;
                int by = (ADDRESS_BAR_H - 22) / 2;
                if (hitButton_(bx, by, 26, 22, mx, my)) { back();    return true; }
                bx += 28;
                if (hitButton_(bx, by, 26, 22, mx, my)) { forward(); return true; }
                bx += 28;
                if (hitButton_(bx, by, 26, 22, mx, my)) { reload();  return true; }
                bx += 28 + 4;

                // Bookmark star (left of the URL field).
                if (hitButton_(bx, by, 24, 22, mx, my)) {
                    toggleBookmark_();
                    return true;
                }
                bx += 28;

                // Click on the URL field — enter editing mode.
                int fx, fy, fw, fh;
                addressBarFieldRect_(fx, fy, fw, fh);
                if (hitButton_(fx, fy, fw, fh, mx, my)) {
                    addressEditing_ = true;
                    addressInput_ = currentPath_;
                    addressCaret_ = (int)addressInput_.size();
                    SDL_StartTextInput();
                    return true;
                } else if (addressEditing_) {
                    addressEditing_ = false;
                    addressInput_.clear();
                    SDL_StopTextInput();
                }

                // Bookmarks bar buttons.
                if (bookmarksVisible_) {
                    int idx = 0;
                    for (auto& r : bookmarkRects_) {
                        if (mx >= r.x && mx < r.x + r.w && my >= r.y && my < r.y + r.h) {
                            if (idx < (int)bookmarks_.size()) {
                                navigate(bookmarks_[idx].url);
                            }
                            return true;
                        }
                        ++idx;
                    }
                }

                // Content area: actionable targets (links, widgets,
                // onclick) keep the old mousedown-activates behavior;
                // anything else starts a text-selection drag. A plain
                // click that never drags just clears the selection.
                if (my >= contentY() && my < contentY() + contentH()) {
                    int cx = mx, cy = my - contentY();
                    if (e.button.clicks >= 2 && !isActionableHit_(cx, cy)) {
                        selectWordAt_(cx, cy);
                    } else if (isActionableHit_(cx, cy)) {
                        clearSelection_();
                        handleClick(cx, cy);
                    } else {
                        startSelection_(cx, cy);
                    }
                }
                return true;
            }
            // Mouse back / forward buttons (X1 = back, X2 = forward).
            if (e.button.button == SDL_BUTTON_X1) { back();    return true; }
            if (e.button.button == SDL_BUTTON_X2) { forward(); return true; }
            break;

        case SDL_MOUSEBUTTONUP:
            if (e.button.button == SDL_BUTTON_LEFT) {
                draggingScrollbar_ = false;
                seekDragBox_ = -1;      // stop scrubbing (if any)
                viewerPanning_ = false;
                // End of a selection drag: keep the highlight when the
                // drag covered text; a click without drag on plain text
                // just clears the previous selection.
                if (selecting_) finishSelection_();
            }
            break;

        case SDL_WINDOWEVENT:
            if (e.window.event == SDL_WINDOWEVENT_RESIZED ||
                e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                winW_ = e.window.data1;
                winH_ = e.window.data2;
                relayout_();
            }
            // Expose/restore/shown: X discarded our pixels while another
            // window (e.g. a fullscreen mpv) covered us. The blanket
            // frameDirty_ above already schedules a repaint for ANY event;
            // these cases are spelled out because they are the ones that
            // actually leave the screen garbled on minimal WMs like dwm if
            // the repaint is ever skipped.
            if (e.window.event == SDL_WINDOWEVENT_EXPOSED ||
                e.window.event == SDL_WINDOWEVENT_RESTORED ||
                e.window.event == SDL_WINDOWEVENT_SHOWN) {
                frameDirty_ = true;
            }
            break;
    }
    return true;
}

// Activate a node programmatically (Enter/Space on a focused button).
void Browser::handleClickForNode_(std::shared_ptr<Node> node) {
    if (!node) return;
    if (node->tag == "button") {
        std::string onclick = findAttr(node, "onclick");
        if (!onclick.empty()) js_.executeEvent(onclick, "click", node);
        js_.dispatchEvent("click", node);
        return;
    }
    if (node->tag == "select") {
        std::vector<std::shared_ptr<Node>> opts;
        for (auto& c : node->children)
            if (c->tag == "option") opts.push_back(c);
        if (opts.size() > 1) {
            size_t cur = 0;
            for (size_t i = 0; i < opts.size(); ++i)
                if (opts[i]->attrs.count("selected")) { cur = i; break; }
            opts[cur]->attrs.erase("selected");
            opts[(cur + 1) % opts.size()]->attrs["selected"] = "selected";
            js_.dispatchEvent("change", node);
            needsRelayout_ = true;
        }
        return;
    }
    // Fall back to hit-testing the node's own box.
    for (auto& b : layout_.boxes) {
        if (b.sourceNode.lock() == node) {
            handleClick(b.x, b.y - scrollY_ + contentY() - contentY());
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------

void Browser::paint() {
    const bool __pb = getenv("MB_PAINTBENCH") != nullptr;
    Uint32 __pb0 = __pb ? SDL_GetTicks() : 0;
    const char* __pbS = getenv("MB_SCROLLSIM");
    int __savedScroll = 0;
    if (__pbS) { __savedScroll = scrollY_; scrollY_ = atoi(__pbS); }
    // Pump JS timers first. If any fire they may set needsRelayout_ via
    // onDomMutated/onInvalidate, which the relayout block below then
    // consumes in the same frame.
    js_.pumpTimers();

    if (needsRelayout_) {
        relayout_();
        needsRelayout_ = false;
    }

    // Debug aid: MB_DUMP=1 prints wide boxes that would draw
    // borders (handy for hunting stray rules on real sites).
    // MB_DUMP=2 also dumps text runs of boxes in MB_DUMP_Y0..Y1.
    if (getenv("MB_DUMP")) {
        int dy0 = getenv("MB_DUMP_Y0") ? atoi(getenv("MB_DUMP_Y0")) : 0;
        int dy1 = getenv("MB_DUMP_Y1") ? atoi(getenv("MB_DUMP_Y1")) : 0;
        for (size_t i = 0; i < layout_.boxes.size(); ++i) {
            const auto& bx = layout_.boxes[i];
            if (std::string(getenv("MB_DUMP")) == "2") {
                if (bx.y + bx.h < dy0 || bx.y > dy1) continue;
                std::string tag = bx.sourceNode.lock() ? bx.sourceNode.lock()->tag : "?";
                std::cerr << "[box " << i << "] <" << tag << "> x=" << bx.x
                          << " y=" << bx.y << " w=" << bx.w << " h=" << bx.h
                          << " lines=" << bx.lines.size() << "\n";
                for (size_t li = 0; li < bx.lines.size() && li < 8; ++li) {
                    std::cerr << "  line" << li << ":";
                    for (auto& r : bx.lines[li]) {
                        std::string t = r.text.substr(0, 40);
                        std::cerr << " '" << t << "'"
                                  << (r.isLink ? "L" : "")
                                  << (r.style.underline ? "U" : "");
                    }
                    std::cerr << "\n";
                }
                continue;
            }
            if (bx.style.border.horizontal() == 0 &&
                bx.style.border.vertical() == 0) continue;
            if (bx.w < 500) continue;
            std::string tag = bx.sourceNode.lock() ? bx.sourceNode.lock()->tag : "?";
            std::string cls = bx.sourceNode.lock() && bx.sourceNode.lock()->attrs.count("class")
                              ? bx.sourceNode.lock()->attrs["class"] : "";
            std::cerr << "[box " << i << "] <" << tag << " class='" << cls
                      << "'> x=" << bx.x << " y=" << bx.y << " w=" << bx.w
                      << " h=" << bx.h
                      << " border(t=" << bx.style.border.top
                      << " b=" << bx.style.border.bottom << ")\n";
        }
    }

    // Clear the whole window to the chrome color.
    SDL_SetRenderDrawColor(ren_, 240, 240, 240, 255);
    SDL_RenderClear(ren_);

    // 1) Page content (clipped to the content area).
    SDL_Rect clip = { contentX(), contentY(), contentW(), contentH() };
    SDL_RenderSetClipRect(ren_, &clip);

    // Background of content area
    SDL_SetRenderDrawColor(ren_, 255, 255, 255, 255);
    SDL_Rect cbg = { contentX(), contentY(), contentW(), contentH() };
    SDL_RenderFillRect(ren_, &cbg);

    // A body/background box must cover the WHOLE viewport, not just the
    // document height — real browsers propagate the body background to
    // the canvas, so short pages stay fully tinted.
    if (!layout_.boxes.empty()) {
        auto& first = layout_.boxes.front();
        auto sn = first.sourceNode.lock();
        if (sn && sn->tag == "body" && first.h < contentH() + 40) {
            first.h = contentH() + 40;
        }
    }

    // Boxes are laid out with y starting at 0 in content coords; pass
    // scrollY_ - contentY() so they render at contentY() + (b.y - scrollY_).
    std::vector<SelectionSpan> selSpans = selectionSpans_();
    // v2.18: render timing log so we can diagnose text-heavy page
    // performance. The render phase (drawing all visible boxes + text
    // textures) is where text-heavy pages spend most of their time.
    auto __renderT0 = std::chrono::steady_clock::now();
    renderWithFocus(ren_, font_, layout_.boxes, scrollY_ - contentY(),
                    hoveredHref_,
                    finding_ ? findMatches_ : std::vector<std::pair<int,int>>{},
                    finding_ ? (int)findCurrent_ : -1,
                    focusedNode_,
                    focusedNode_ ? focusedCaret() : -1,
                    selSpans.empty() ? nullptr : &selSpans);
    if (!paintedOnce_)
        std::cerr << "[perf] render " << msSince(__renderT0) << "ms\n";

    SDL_RenderSetClipRect(ren_, nullptr);

    // 1.5) Image viewer overlay: covers the content area; the chrome
    // (address bar etc.) stays visible and functional above it.
    if (viewerOpen_) drawImageViewer_();

    // 2) Chrome (address bar, bookmarks bar, status bar, scrollbar).
    drawChrome_();

    // 3) Present the composed frame. The content pass (renderWithFocus)
    // no longer presents mid-frame — doing so pushed the frame to the
    // screen BEFORE the chrome was drawn, and the next frame's clear
    // wiped the chrome before it ever reached the display. Under real
    // window managers (dwm etc.) the browser appeared chrome-less.
    SDL_RenderPresent(ren_);

    // Frame is on screen: the event loop may now run deferred work and
    // then sleeps until something actually changes (see tick() /
    // nextWakeupMs()) instead of repainting at 60 fps forever.
    paintedOnce_ = true;
    frameDirty_ = false;
    if (__pb)
        std::cerr << "[paintbench] paint " << (SDL_GetTicks() - __pb0)
                  << " ms (scrollY=" << scrollY_ << " boxes="
                  << layout_.boxes.size() << ")\n";
    if (__pbS) scrollY_ = __savedScroll;
}

void Browser::drawChrome_() {
    drawAddressBar_();
    drawBookmarksBar_();
    drawFindBar_();
    drawStatusBar_();
    drawScrollbar_();
    drawAlert_();
}

void Browser::drawButton_(int x, int y, int w, int h,
                          const std::string& label, bool enabled, bool hovered) {
    SDL_Color bg   = enabled ? (hovered ? SDL_Color{220,220,220,255}
                                        : SDL_Color{240,240,240,255})
                             : SDL_Color{235,235,235,255};
    SDL_Color fg   = enabled ? SDL_Color{40,40,40,255} : SDL_Color{170,170,170,255};
    SDL_Color bord = enabled ? SDL_Color{160,160,160,255} : SDL_Color{200,200,200,255};
    SDL_SetRenderDrawColor(ren_, bg.r, bg.g, bg.b, 255);
    SDL_Rect r = {x, y, w, h};
    SDL_RenderFillRect(ren_, &r);
    SDL_SetRenderDrawColor(ren_, bord.r, bord.g, bord.b, 255);
    SDL_RenderDrawRect(ren_, &r);

    if (font_) {
        int tw = 0, th = 0;
        SDL_Texture* t = cachedTextTexture(ren_, font_, label, fg, &tw, &th);
        if (t) {
            SDL_Rect dst = { x + (w - tw)/2, y + (h - th)/2, tw, th };
            SDL_RenderCopy(ren_, t, nullptr, &dst);
        }
    }
}

void Browser::drawAddressBar_() {
    // Background of the address bar
    SDL_SetRenderDrawColor(ren_, 232, 232, 236, 255);
    SDL_Rect bar = {0, 0, winW_, ADDRESS_BAR_H};
    SDL_RenderFillRect(ren_, &bar);
    SDL_SetRenderDrawColor(ren_, 200, 200, 200, 255);
    SDL_RenderDrawLine(ren_, 0, ADDRESS_BAR_H, winW_, ADDRESS_BAR_H);

    // Back / Forward / Reload buttons
    int bx = 4;
    int by = (ADDRESS_BAR_H - 22) / 2;
    bool backHover    = hitButton_(bx, by, 26, 22, mouseX_, mouseY_);
    drawButton_(bx, by, 26, 22, "<", canGoBack(), backHover && canGoBack());
    bx += 28;
    bool fwdHover     = hitButton_(bx, by, 26, 22, mouseX_, mouseY_);
    drawButton_(bx, by, 26, 22, ">", canGoForward(), fwdHover && canGoForward());
    bx += 28;
    bool reloadHover  = hitButton_(bx, by, 26, 22, mouseX_, mouseY_);
    drawButton_(bx, by, 26, 22, "R", !history_.empty(), reloadHover && !history_.empty());
    bx += 28 + 4;

    // Bookmark star: filled when the current page is bookmarked.
    bool marked = isBookmarked_();
    SDL_Color starBg = marked ? SDL_Color{255, 220, 120, 255}
                              : SDL_Color{240, 240, 240, 255};
    SDL_SetRenderDrawColor(ren_, starBg.r, starBg.g, starBg.b, 255);
    SDL_Rect star = {bx, by, 24, 22};
    SDL_RenderFillRect(ren_, &star);
    SDL_SetRenderDrawColor(ren_, 160, 160, 160, 255);
    SDL_RenderDrawRect(ren_, &star);
    if (font_) {
        SDL_Color starCol = marked ? SDL_Color{180, 120, 0, 255}
                                   : SDL_Color{120,120,120,255};
        int sw = 0, sh = 0;
        SDL_Texture* t = cachedTextTexture(ren_, font_, "*", starCol, &sw, &sh);
        if (t) {
            SDL_Rect dst = { bx + (24 - sw)/2, by + (22 - sh)/2, sw, sh };
            SDL_RenderCopy(ren_, t, nullptr, &dst);
        }
    }
    bx += 28;

    // URL field
    int fx, fy, fw, fh;
    addressBarFieldRect_(fx, fy, fw, fh);
    bool fieldHover = hitButton_(fx, fy, fw, fh, mouseX_, mouseY_);

    SDL_SetRenderDrawColor(ren_, 255, 255, 255, 255);
    SDL_Rect field = {fx, fy, fw, fh};
    SDL_RenderFillRect(ren_, &field);
    // Border: blue when editing, gray otherwise.
    if (addressEditing_) {
        SDL_SetRenderDrawColor(ren_, 0x1a, 0x4f, 0xa0, 255);
    } else if (fieldHover) {
        SDL_SetRenderDrawColor(ren_, 120, 160, 220, 255);
    } else {
        SDL_SetRenderDrawColor(ren_, 180, 180, 180, 255);
    }
    SDL_RenderDrawRect(ren_, &field);

    // Draw the text inside the field.
    std::string text = addressEditing_ ? addressInput_ : currentPath_;
    if (font_) {
        int tw = 0, th = 0;
        SDL_Texture* t = cachedTextTexture(ren_, font_, text,
                                           {40, 40, 40, 255}, &tw, &th);
        if (t) {
            int availW = fw - 12;
            int drawW = std::min(tw, availW);
            SDL_Rect src = {0, 0, drawW, th};
            SDL_Rect dst = { fx + 6, fy + (fh - th)/2, drawW, th };
            SDL_RenderCopy(ren_, t, &src, &dst);
        }
    }

    // Blinking caret when editing (at the caret offset, not just the end).
    if (addressEditing_) {
        cursorBlinkTick_++;
        if ((cursorBlinkTick_ / 30) % 2 == 0) {
            int caretX = fx + 6;
            if (font_) {
                int caret = std::clamp(addressCaret_, 0, (int)addressInput_.size());
                std::string before = addressInput_.substr(0, (size_t)caret);
                int textW = 0, textH = 0;
                TTF_SizeUTF8(font_, before.c_str(), &textW, &textH);
                caretX += std::min(textW, fw - 12);
            }
            SDL_SetRenderDrawColor(ren_, 0x1a, 0x4f, 0xa0, 255);
            SDL_Rect caret = {caretX, fy + 4, 1, fh - 8};
            SDL_RenderFillRect(ren_, &caret);
        }
    }
}

void Browser::addressBarFieldRect_(int& x, int& y, int& w, int& h) const {
    int bx = 4 + 28 + 28 + 28 + 4 + 28 + 28;  // 3 buttons + star + spacings
    x = bx;
    y = (ADDRESS_BAR_H - 22) / 2;
    w = winW_ - bx - 8;
    h = 22;
}

void Browser::drawBookmarksBar_() {
    bookmarkRects_.clear();
    if (!bookmarksVisible_) return;
    int y = ADDRESS_BAR_H;
    SDL_SetRenderDrawColor(ren_, 226, 228, 234, 255);
    SDL_Rect bar = {0, y, winW_, BOOKMARKS_H};
    SDL_RenderFillRect(ren_, &bar);
    SDL_SetRenderDrawColor(ren_, 200, 200, 200, 255);
    SDL_RenderDrawLine(ren_, 0, y + BOOKMARKS_H, winW_, y + BOOKMARKS_H);

    int x = 6;
    if (font_) {
        for (auto& b : bookmarks_) {
            std::string label = b.title.empty() ? b.url : b.title;
            if (label.size() > 24) label = label.substr(0, 24) + "...";
            int lw = 0, lh = 0;
            SDL_Texture* t = cachedTextTexture(ren_, font_, label,
                                               {40, 40, 60, 255}, &lw, &lh);
            if (!t) continue;
            int bw = std::min(lw + 12, 220);
            if (x + bw > winW_ - 8) break;
            bool hov = hitButton_(x, y + 2, bw, BOOKMARKS_H - 4, mouseX_, mouseY_);
            SDL_SetRenderDrawColor(ren_, hov ? Uint8(210) : Uint8(238),
                                   hov ? Uint8(210) : Uint8(238),
                                   hov ? Uint8(222) : Uint8(242), 255);
            SDL_Rect r = {x, y + 2, bw, BOOKMARKS_H - 4};
            SDL_RenderFillRect(ren_, &r);
            SDL_Rect dst = { x + 6, y + (BOOKMARKS_H - lh)/2, lw, lh };
            SDL_RenderCopy(ren_, t, nullptr, &dst);
            bookmarkRects_.push_back({x, y + 2, bw, BOOKMARKS_H - 4});
            x += bw + 6;
        }
    }
    if (bookmarks_.empty() && font_) {
        int lw = 0, lh = 0;
        SDL_Texture* t = cachedTextTexture(
            ren_, font_, "No bookmarks yet - press Ctrl+D to add this page",
            {130, 130, 130, 255}, &lw, &lh);
        if (t) {
            SDL_Rect dst = { 8, y + (BOOKMARKS_H - lh)/2, lw, lh };
            SDL_RenderCopy(ren_, t, nullptr, &dst);
        }
    }
}

void Browser::scrollbarThumbRect_(int& x, int& y, int& w, int& h) const {
    int total = layout_.contentHeight;
    int visible = contentH();
    int trackY = contentY();
    int trackH = contentH();
    if (total <= visible) {
        x = winW_ - SCROLLBAR_W;
        y = trackY;
        w = SCROLLBAR_W;
        h = 0;
        return;
    }
    int thumbH = std::max(20, trackH * visible / total);
    int thumbY = trackY + (trackH - thumbH) * scrollY_ / std::max(1, total - visible);
    x = winW_ - SCROLLBAR_W;
    y = thumbY;
    w = SCROLLBAR_W;
    h = thumbH;
}

// ---------------------------------------------------------------------------
// Find-in-page
// ---------------------------------------------------------------------------

// Case-insensitive substring search.
static bool containsCI(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return false;
    auto lower = [](std::string s) {
        for (auto& c : s) c = (char)tolower((unsigned char)c);
        return s;
    };
    std::string h = lower(haystack);
    std::string n = lower(needle);
    return h.find(n) != std::string::npos;
}

void Browser::runFind_() {
    findMatches_.clear();
    findCurrent_ = 0;
    if (findQuery_.empty()) return;

    // Walk every text run in every box; record (boxIdx, lineIdx) for
    // each line that contains the query (case-insensitive). We track
    // at the line level because we can easily scroll to a line via
    // the box's y position.
    for (size_t bi = 0; bi < layout_.boxes.size(); ++bi) {
        const Box& b = layout_.boxes[bi];
        for (size_t li = 0; li < b.lines.size(); ++li) {
            for (const auto& r : b.lines[li]) {
                // Prefer the logical text: RTL runs store visual order
                // in `text`, which would never match a logical query.
                const std::string& hay = r.logical.empty() ? r.text : r.logical;
                if (containsCI(hay, findQuery_)) {
                    findMatches_.push_back({(int)bi, (int)li});
                    break;  // one match per line is enough for scrolling
                }
            }
        }
    }

    if (!findMatches_.empty()) {
        findCurrent_ = 0;
        // Scroll the first match into view.
        const auto& m = findMatches_[0];
        if (m.first < (int)layout_.boxes.size()) {
            const Box& b = layout_.boxes[m.first];
            int lineH = (b.style.hasLineHeight && b.style.lineHeight > 0)
                          ? (int)b.style.lineHeight : b.style.fontSize + 6;
            int lineY = b.y + m.second * lineH;
            scrollY_ = std::clamp(lineY - contentY(), 0, maxScroll());
        }
    }
}

void Browser::findNext_(int dir) {
    if (findMatches_.empty()) return;
    findCurrent_ = (findCurrent_ + dir + findMatches_.size()) % findMatches_.size();
    const auto& m = findMatches_[findCurrent_];
    if (m.first < (int)layout_.boxes.size()) {
        const Box& b = layout_.boxes[m.first];
        int lineH = (b.style.hasLineHeight && b.style.lineHeight > 0)
                      ? (int)b.style.lineHeight : b.style.fontSize + 6;
        int lineY = b.y + m.second * lineH;
        scrollY_ = std::clamp(lineY - contentY(), 0, maxScroll());
    }
}

// ---------------------------------------------------------------------------
// Focus management
// ---------------------------------------------------------------------------

std::vector<std::shared_ptr<Node>> Browser::collectFocusable_() const {
    std::vector<std::shared_ptr<Node>> out;
    if (!dom_) return out;
    std::function<void(const std::shared_ptr<Node>&)> walk =
        [&](const std::shared_ptr<Node>& n) {
            for (auto& c : n->children) {
                bool focusable = false;
                if (c->tag == "input" || c->tag == "textarea" ||
                    c->tag == "button" || c->tag == "select") {
                    focusable = true;
                }
                if (c->tag == "a" && c->attrs.count("href")) focusable = true;
                if (focusable) out.push_back(c);
                walk(c);
            }
        };
    walk(dom_);
    return out;
}

void Browser::advanceFocus_(bool reverse) {
    auto els = collectFocusable_();
    if (els.empty()) {
        focusedNode_.reset();
        SDL_StopTextInput();
        return;
    }
    if (!focusedNode_) {
        focusedNode_ = reverse ? els.back() : els.front();
        if (focusedNode_->tag == "input" || focusedNode_->tag == "textarea") {
            SDL_StartTextInput();
        }
        needsRelayout_ = true;
        return;
    }
    // Find current position in the list.
    size_t idx = 0; bool found = false;
    for (size_t i = 0; i < els.size(); ++i) {
        if (els[i].get() == focusedNode_.get()) { idx = i; found = true; break; }
    }
    if (!found) {
        focusedNode_ = reverse ? els.back() : els.front();
    } else {
        if (reverse) {
            focusedNode_ = (idx == 0) ? els.back() : els[idx - 1];
        } else {
            focusedNode_ = (idx + 1 == els.size()) ? els.front() : els[idx + 1];
        }
    }
    if (focusedNode_->tag == "input" || focusedNode_->tag == "textarea") {
        SDL_StartTextInput();
    } else {
        SDL_StopTextInput();
    }
    needsRelayout_ = true;
}

// ---------------------------------------------------------------------------
// Status bar / scrollbar / find bar / alert
// ---------------------------------------------------------------------------

void Browser::drawStatusBar_() {
    int y = winH_ - STATUS_BAR_H;
    SDL_SetRenderDrawColor(ren_, 244, 244, 248, 255);
    SDL_Rect bar = {0, y, winW_, STATUS_BAR_H};
    SDL_RenderFillRect(ren_, &bar);
    SDL_SetRenderDrawColor(ren_, 200, 200, 200, 255);
    SDL_RenderDrawLine(ren_, 0, y, winW_, y);

    std::string msg = hoveredHref_.empty() ? currentPath_ : hoveredHref_;
    if (font_) {
        int mw = 0, mh = 0;
        // Shrink the left URL so it can never collide with the (often
        // long) right-side status message, e.g. the mpv yt-dlp guidance.
        int maxMsgW = winW_ - 8 /*left pad*/ - 8 /*right pad*/;
        {
            int rw0 = 0, rh0 = 0;
            std::string right;
            if (!pageInfo_.empty()) right = pageInfo_;
            char zb0[32];
            snprintf(zb0, sizeof(zb0), "%d%%", (int)std::lround(zoom_ * 100));
            right = right.empty() ? std::string(zb0) : (right + " \xC2\xB7 " + zb0);
            TTF_SizeUTF8(font_, right.c_str(), &rw0, &rh0);
            maxMsgW -= rw0;
        }
        // Middle-out truncation: keep the scheme and the tail (TLD/path).
        while (TTF_SizeUTF8(font_, msg.c_str(), &mw, &mh) == 0 && mw > maxMsgW &&
               msg.size() > 12) {
            msg = msg.substr(0, msg.size() / 2 - 2) + "\xE2\x80\xA6" +
                  msg.substr(msg.size() - 5);
        }
        SDL_Texture* t = cachedTextTexture(ren_, font_, msg,
                                           {80, 80, 80, 255}, &mw, &mh);
        if (t) {
            SDL_Rect dst = { 6, y + (STATUS_BAR_H - mh)/2, mw, mh };
            SDL_RenderCopy(ren_, t, nullptr, &dst);
        }
    }

    // Right side: page info (HTTP status / content type / size) + zoom.
    std::string right;
    if (!pageInfo_.empty()) right = pageInfo_;
    char zb[32];
    snprintf(zb, sizeof(zb), "%d%%", (int)std::lround(zoom_ * 100));
    right = right.empty() ? std::string(zb) : (right + " · " + zb);
    if (font_ && !right.empty()) {
        int rw = 0, rh = 0;
        SDL_Texture* t = cachedTextTexture(ren_, font_, right,
                                           {120, 120, 130, 255}, &rw, &rh);
        if (t) {
            SDL_Rect dst = { winW_ - rw - 8, y + (STATUS_BAR_H - rh)/2,
                             rw, rh };
            SDL_RenderCopy(ren_, t, nullptr, &dst);
        }
    }
}

void Browser::drawScrollbar_() {
    int total = layout_.contentHeight;
    int visible = contentH();
    if (total <= visible) return;

    int tx, ty, tw, th;
    scrollbarThumbRect_(tx, ty, tw, th);

    // Track background
    int trackX = winW_ - SCROLLBAR_W;
    int trackY = contentY();
    int trackH = contentH();
    SDL_SetRenderDrawColor(ren_, 230, 230, 230, 255);
    SDL_Rect track = {trackX, trackY, SCROLLBAR_W, trackH};
    SDL_RenderFillRect(ren_, &track);

    // Thumb (highlight when being dragged or hovered)
    bool thumbHover = hitButton_(tx, ty, tw, th, mouseX_, mouseY_);
    Uint8 shade = (draggingScrollbar_ || thumbHover) ? 130 : 170;
    SDL_SetRenderDrawColor(ren_, shade, shade, shade, 255);
    SDL_Rect thumb = {tx + 2, ty, tw - 4, th};
    SDL_RenderFillRect(ren_, &thumb);
}

void Browser::drawFindBar_() {
    if (!finding_) return;
    // Find bar lives just above the status bar.
    int barH = 28;
    int by = winH_ - STATUS_BAR_H - barH;
    SDL_SetRenderDrawColor(ren_, 240, 240, 244, 255);
    SDL_Rect bar = {0, by, winW_, barH};
    SDL_RenderFillRect(ren_, &bar);
    SDL_SetRenderDrawColor(ren_, 200, 200, 200, 255);
    SDL_RenderDrawLine(ren_, 0, by, winW_, by);

    std::string label = "Find: " + findQuery_;
    if (findMatches_.empty() && !findQuery_.empty()) label += "  (no matches)";
    else if (!findQuery_.empty()) {
        label += "  (" + std::to_string(findCurrent_ + 1) +
                 "/" + std::to_string(findMatches_.size()) + ")";
    }
    if (font_) {
        int lw = 0, lh = 0;
        SDL_Texture* t = cachedTextTexture(ren_, font_, label,
                                           {40, 40, 40, 255}, &lw, &lh);
        if (t) {
            SDL_Rect dst = { 8, by + (barH - lh)/2, lw, lh };
            SDL_RenderCopy(ren_, t, nullptr, &dst);
        }
    }
    // Hint on the right side
    if (font_) {
        const char* hint = "Enter=next  Shift+Enter=prev  Esc=close";
        int hw = 0, hh = 0;
        SDL_Texture* t = cachedTextTexture(ren_, font_, hint,
                                           {120,120,120,255}, &hw, &hh);
        if (t) {
            SDL_Rect dst = { winW_ - hw - 8, by + (barH - hh)/2, hw, hh };
            SDL_RenderCopy(ren_, t, nullptr, &dst);
        }
    }
}

void Browser::drawAlert_() {
    if (!alertVisible_) return;
    // Dim overlay.
    SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(ren_, 0, 0, 0, 90);
    SDL_Rect full = {0, 0, winW_, winH_};
    SDL_RenderFillRect(ren_, &full);
    SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_NONE);

    // Dialog box.
    int dw = std::min(winW_ - 80, 440);
    int dh = 120;
    int dx = (winW_ - dw) / 2;
    int dy = (winH_ - dh) / 2;
    SDL_SetRenderDrawColor(ren_, 250, 250, 252, 255);
    SDL_Rect box = {dx, dy, dw, dh};
    SDL_RenderFillRect(ren_, &box);
    SDL_SetRenderDrawColor(ren_, 60, 60, 70, 255);
    SDL_RenderDrawRect(ren_, &box);
    SDL_Rect box2 = {dx-1, dy-1, dw+2, dh+2};
    SDL_RenderDrawRect(ren_, &box2);

    if (font_) {
        std::string title = "The page says:";
        int tw2 = 0, th2 = 0;
        SDL_Texture* t = cachedTextTexture(ren_, font_, title,
                                           {60, 60, 70, 255}, &tw2, &th2);
        if (t) {
            SDL_Rect dst = { dx + 14, dy + 10, tw2, th2 };
            SDL_RenderCopy(ren_, t, nullptr, &dst);
        }
        std::string msg = alertText_;
        if (msg.size() > 64) msg = msg.substr(0, 64) + "...";
        int mw = 0, mh = 0;
        SDL_Texture* mt = cachedTextTexture(ren_, font_, msg,
                                            {20, 20, 30, 255}, &mw, &mh);
        if (mt) {
            SDL_Rect dst = { dx + 14, dy + 34, std::min(mw, dw - 28), mh };
            SDL_RenderCopy(ren_, mt, nullptr, &dst);
        }
    }
    // OK button.
    int bw = 80, bh = 26;
    int bx = dx + dw - bw - 12;
    int byy = dy + dh - bh - 12;
    alertOkRect_ = {bx, byy, bw, bh};
    bool hov = hitButton_(bx, byy, bw, bh, mouseX_, mouseY_);
    drawButton_(bx, byy, bw, bh, "OK", true, hov);
}

// ---------------------------------------------------------------------------
// Bookmarks
// ---------------------------------------------------------------------------

static std::string bookmarksPath() {
    const char* home = getenv("HOME");
    return (home ? std::string(home) : std::string(".")) +
           "/.minibrowser-bookmarks.txt";
}

void Browser::loadBookmarks_() {
    bookmarks_.clear();
    std::ifstream f(bookmarksPath());
    if (!f) return;
    std::string line;
    while (std::getline(f, line)) {
        auto tab = line.find('\t');
        Bookmark b;
        if (tab != std::string::npos) {
            b.url = line.substr(0, tab);
            b.title = line.substr(tab + 1);
        } else {
            b.url = line;
        }
        if (!b.url.empty()) bookmarks_.push_back(b);
    }
}

void Browser::saveBookmarks_() const {
    std::ofstream f(bookmarksPath());
    if (!f) return;
    for (auto& b : bookmarks_) {
        f << b.url << "\t" << b.title << "\n";
    }
}

bool Browser::isBookmarked_() const {
    for (auto& b : bookmarks_)
        if (b.url == currentPath_) return true;
    return false;
}

void Browser::toggleBookmark_() {
    for (size_t i = 0; i < bookmarks_.size(); ++i) {
        if (bookmarks_[i].url == currentPath_) {
            bookmarks_.erase(bookmarks_.begin() + i);
            saveBookmarks_();
            return;
        }
    }
    Bookmark b;
    b.url = currentPath_;
    b.title = findDocTitle(dom_);
    bookmarks_.push_back(b);
    saveBookmarks_();
}

// ---------------------------------------------------------------------------
// Save page (Ctrl+S)
// ---------------------------------------------------------------------------

void Browser::savePage_() {
    if (rawSource_.empty()) return;
    std::string name = findDocTitle(dom_);
    if (name.empty() || name == "Minimal Browser") name = "page";
    for (auto& c : name)
        if (!(std::isalnum((unsigned char)c) || c == '-' || c == '_' ||
              c == '.' || c == ' ')) c = '_';
    std::string path = name + ".html";
    std::ofstream f(path, std::ios::binary);
    if (f) {
        f << rawSource_;
        pageInfo_ = "Saved to " + path;
        std::cout << "[save] wrote " << path << "\n";
    } else {
        pageInfo_ = "Save failed: " + path;
    }
}

} // namespace browser



