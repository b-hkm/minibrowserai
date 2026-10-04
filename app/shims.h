#pragma once
#include <string>

namespace browser {
namespace shims {

// ---------------------------------------------------------------------------
// Site shims (v2.8: hybrid mode).
//
// History:
//   - v2.4-v2.6: the engine sent a "MiniBrowser/2.6" User-Agent. Sites
//     treated it as a bot and shipped minimal / "needs JS" pages. Two
//     shims worked around that:
//       * web search: google.com/search was rewritten to lite.duckduckgo.com/lite
//         (fully server-rendered in a few KB).
//       * YouTube: the page's ytInitialData / ytInitialPlayerResponse JSON
//         was scraped and re-rendered as a static "YouTube Lite" page.
//   - v2.7: net/fetch.cpp switched to a real Chrome User-Agent + the
//     Sec-Fetch-*, Sec-CH-UA, Upgrade-Insecure-Requests and Accept headers
//     a real Chrome navigation would send. All shims were removed on the
//     assumption that the Chrome UA alone would make the actual services
//     render — but YouTube / Google / DuckDuckGo are all JavaScript SPAs,
//     and the engine's Duktape ES5.1 interpreter cannot run their modern
//     ES6+ code, so the SPA shells loaded but rendered blank (just the
//     footer / chrome).
//   - v2.8: hybrid. The Chrome UA stays (it makes sites ship the real
//     HTML, with the ytInitialData JSON reliably present on YouTube).
//     The YouTube data-extractor is REINTRODUCED in app/shims.cpp — it
//     reads the ytInitialData / ytInitialPlayerResponse JSON already
//     embedded in the page and renders a clean static page, branded
//     just "YouTube" (NOT "YouTube Lite" — it is the actual page's own
//     data, not an alternative site). Address-bar search goes to the
//     REAL server-rendered DuckDuckGo at html.duckduckgo.com/html (NOT
//     lite.duckduckgo.com/lite — that was the "lite" alternative the
//     user explicitly asked to remove in v2.7; html.duckduckgo.com/html
//     is the same DuckDuckGo service, just server-rendered HTML).
//
// All of the below remain controllable via MB_NOSHIM=1 for parity with the
// previous API.
// ---------------------------------------------------------------------------

// True when the address-bar input is a web-search phrase rather than a
// URL, bare domain or file path (has spaces, or no dot/scheme/path chars).
bool looksLikeSearchQuery(const std::string& input);

// v2.8: real server-rendered DuckDuckGo at https://html.duckduckgo.com/html
// (NOT lite.duckduckgo.com/lite — that was the "lite" alternative the
// user asked to remove; html.duckduckgo.com/html is the same DDG service,
// just server-rendered HTML, no JS required, so the engine renders the
// results list directly).
std::string webSearchUrl(const std::string& query);

// v2.8: still returns "" (no rewrite). google.com/search is fetched as-is;
// the Chrome UA + Sec-Fetch-* headers in net/fetch.cpp make Google serve
// its real search page (note: Google's real search page is a JS SPA so
// it still renders mostly blank — the user explicitly asked NOT to
// rewrite google.com/search, so we honour that; the address-bar search
// box goes to html.duckduckgo.com/html which DOES render).
std::string maybeRewriteSearchUrl(const std::string& url);

bool isYouTubeUrl(const std::string& url);

// v2.8: REINTRODUCED. Reads the ytInitialData / ytInitialPlayerResponse
// JSON already embedded in the YouTube page (the Chrome UA in net/fetch.cpp
// guarantees it is there) and renders a clean static HTML page — search
// box + thumbnail grid on home / search pages; title / description /
// related videos + Play-in-browser / Play-in-mpv chips on watch pages.
//
// The page is branded just "YouTube" (NOT "YouTube Lite" — it is the
// actual page's own data, not an alternative site; the JS engine just
// cannot drive the SPA, so we render the data statically).
//
// ok=false tells the caller (fetchDocumentWithShims in app/browser.cpp)
// to keep the original page body — happens when ytInitialData is missing
// or unreadable, which is rare with the Chrome UA but possible behind
// region / consent gates.
//
// `bridgeReason` (kept from v2.6): when non-empty the watch page carries
// an amber banner explaining WHY the in-browser extraction failed (bot
// gate, unavailable video, ...) plus a "Play in browser" retry chip that
// re-runs the bridge. The mpv hand-off chip is only rendered when mpv
// is actually installed.
std::string youTubeLiteHtml(const std::string& url, const std::string& rawHtml,
                            bool& ok, const std::string& bridgeReason = "");

} // namespace shims
} // namespace browser
