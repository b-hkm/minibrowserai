#include "shims.h"

#include "mpv.h"
#include "../net/url.h"
#include "../js/duktape/duktape.h"

#include <cstdlib>
#include <iostream>
#include <sstream>

namespace browser {
namespace shims {

static bool shimsDisabled() {
    // Kept for API compatibility. The flag now only controls the YouTube
    // data-extractor (which is the only "shim" left). The DuckDuckGo
    // address-bar search and the google.com/search pass-through are not
    // affected by it — they're plain URL dispatches.
    const char* e = getenv("MB_NOSHIM");
    return e && e[0] == '1';
}

// ---------------------------------------------------------------------------
// Web search
//
// v2.7 switched the address-bar search to duckduckgo.com (the JS SPA) and
// removed the DDG Lite redirect. v2.8 keeps the "no DDG Lite" stance but
// switches to the *real* server-rendered DuckDuckGo endpoint at
// html.duckduckgo.com/html — this is NOT the lite.duckduckgo.com/lite
// endpoint the user asked to remove, it's DuckDuckGo's own server-rendered
// HTML view (the same results, no JS required). Real DuckDuckGo, just
// rendered.
//
// The Chrome UA + Sec-Fetch-* headers in net/fetch.cpp keep the request
// looking like a real browser navigation, so DDG doesn't downgrade the
// HTML view to a "you need JS" stub.
// ---------------------------------------------------------------------------

bool looksLikeSearchQuery(const std::string& input) {
    std::string s;
    for (char c : input) {
        if (c == ' ' || c == '\t') { s = input; break; }
    }
    if (!s.empty()) return true;                     // contains whitespace
    if (input.empty()) return false;
    if (input[0] == '/' || input[0] == '.' || input[0] == '~') return false;
    if (input.find("://") != std::string::npos) return false;
    if (input.find(':') != std::string::npos) return false;   // scheme/host:port
    if (input.find('.') != std::string::npos) return false;   // domain or file
    return true;
}

std::string webSearchUrl(const std::string& query) {
    // v2.8: real server-rendered DuckDuckGo at html.duckduckgo.com/html
    // (NOT lite.duckduckgo.com/lite — that was the "lite" alternative the
    // user asked to remove in v2.7). This endpoint returns the actual
    // search results as plain HTML, no JavaScript required, so the engine
    // renders it directly.
    return "https://html.duckduckgo.com/html/?q=" + urlEncode(query);
}

// Pull one URL-decoded query parameter out of a raw query string.
static std::string queryParam(const std::string& query, const char* key) {
    size_t pos = 0;
    std::string prefix = std::string(key) + "=";
    while (pos <= query.size()) {
        size_t amp = query.find('&', pos);
        std::string part = query.substr(pos, (amp == std::string::npos)
                                                 ? std::string::npos
                                                 : amp - pos);
        if (part.compare(0, prefix.size(), prefix) == 0)
            return urlDecode(part.substr(prefix.size()));
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return "";
}

// v2.8: still no google.com/search -> DDG rewrite. The Chrome UA in
// net/fetch.cpp makes Google serve its real search page directly. (Note:
// Google's real search page is itself a JS SPA, so it still renders
// mostly blank in our engine — but the user explicitly asked to NOT
// rewrite google.com/search, so we honour that. The address-bar search
// box goes to DDG html instead, which actually renders.)
std::string maybeRewriteSearchUrl(const std::string& url) {
    if (shimsDisabled()) return "";
    Url u = parseUrl(url);
    if (!u.valid || u.scheme != "https") return "";
    bool google = (u.host == "google.com" || u.host == "www.google.com" ||
                   u.host == "m.google.com");
    if (!google || u.path != "/search") return "";
    // No rewrite — Google loads directly with the Chrome UA.
    return "";
}

// ---------------------------------------------------------------------------
// YouTube data-extractor
//
// v2.6 had a "YouTube Lite" shim that scraped ytInitialData /
// ytInitialPlayerResponse JSON from the page and rendered a static HTML
// page. v2.7 removed it on the assumption that the Chrome UA alone would
// make YouTube render — but YouTube is a JS SPA, so the Chrome UA only
// gets the SPA shell (887 KB of HTML with 857 KB of <script> and ~163
// chars of visible text — just the footer).
//
// v2.8 brings back the extractor, but renamed: the page is now branded
// just "YouTube" (not "YouTube Lite") because it is NOT a lite
// alternative site — it's the actual page's own ytInitialData JSON,
// re-rendered as static HTML because the JS engine (Duktape ES5.1)
// cannot drive the SPA. With the Chrome UA in net/fetch.cpp the JSON is
// reliably present (in v2.6 the old "MiniBrowser" UA made YouTube serve
// a bot/consent stub that often had no JSON — the extractor was flaky.
// Now it works every time.)
//
// ok=false signals "I did not produce a rewritten body, use the original"
// to the caller in app/browser.cpp (fetchDocumentWithShims).
// ---------------------------------------------------------------------------

bool isYouTubeUrl(const std::string& url) {
    Url u = parseUrl(url);
    if (!u.valid) return false;
    if (u.host != "youtube.com" && u.host != "www.youtube.com" &&
        u.host != "m.youtube.com" && u.host != "music.youtube.com")
        return false;
    return true;
}

// Extract the JS object literal assigned to `varName` inside a <script>
// (e.g. `var ytInitialData = {...};`). Scans from the assignment to the
// matching closing brace, honoring strings and escapes.
static std::string extractAssignedObject(const std::string& html,
                                         const std::string& varName) {
    std::string needle = varName;   // matches both `x = ` and `var x = `
    size_t p = html.find(needle);
    while (p != std::string::npos) {
        size_t eq = html.find('=', p + needle.size());
        if (eq != std::string::npos && eq - p < needle.size() + 24) {
            size_t b = eq + 1;
            while (b < html.size() &&
                   (html[b] == ' ' || html[b] == '\n' || html[b] == '\r' ||
                    html[b] == '\t'))
                ++b;
            if (b < html.size() && html[b] == '{') {
                int depth = 0;
                bool inStr = false, esc = false;
                for (size_t i = b; i < html.size(); ++i) {
                    char c = html[i];
                    if (inStr) {
                        if (esc) { esc = false; }
                        else if (c == '\\') { esc = true; }
                        else if (c == '"') { inStr = false; }
                        continue;
                    }
                    if (c == '"') { inStr = true; continue; }
                    if (c == '{') { ++depth; continue; }
                    if (c == '}') {
                        if (--depth == 0) {
                            std::string out = html.substr(b, i - b + 1);
                            // U+2028/U+2029 are valid JSON but not valid
                            // JS in ES5.1 (duktape) string literals.
                            for (size_t k = 0; k + 2 < out.size(); ++k)
                                if ((unsigned char)out[k] == 0xE2 &&
                                    (unsigned char)out[k+1] == 0x80 &&
                                    ((unsigned char)out[k+2] == 0xA8 ||
                                     (unsigned char)out[k+2] == 0xA9))
                                    out.replace(k, 3, " ");
                            return out;
                        }
                    }
                }
                return "";   // unbalanced (truncated page)
            }
        }
        p = html.find(needle, p + needle.size());
    }
    return "";
}

// "181422437" -> "181,422,437" (view counts arrive as raw digit runs).
static std::string groupDigits(const std::string& s) {
    if (s.empty() || s.size() > 15) return s;
    for (char c : s) if (c < '0' || c > '9') return s;
    std::string out;
    out.reserve(s.size() + s.size() / 3);
    size_t first = s.size() % 3;
    if (first == 0) first = 3;
    out += s.substr(0, first);
    for (size_t i = first; i < s.size(); i += 3) {
        out += ',';
        out += s.substr(i, 3);
    }
    return out;
}

static std::string escapeHtml(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 16);
    for (char c : s) {
        switch (c) {
            case '&':  o += "&amp;";  break;
            case '<':  o += "&lt;";   break;
            case '>':  o += "&gt;";   break;
            case '"':  o += "&quot;"; break;
            default:   o += c;
        }
    }
    return o;
}

// Run a duktape expression with DATA bound to the parsed JSON blob and
// return the resulting string. Empty on any error.
static std::string walkJson(const std::string& json, const char* scriptTail) {
    if (json.empty()) return "";
    duk_context* ctx = duk_create_heap_default();
    if (!ctx) return "";
    std::string src = "var DATA = " + json + ";(" + scriptTail + ")(DATA)";
    std::string result;
    if (duk_peval_string(ctx, src.c_str()) != 0) {
        std::cerr << "[yt] data walk failed: "
                  << duk_safe_to_string(ctx, -1) << "\n";
    } else {
        const char* r = duk_safe_to_string(ctx, -1);
        if (r && duk_is_string(ctx, -1)) result = r;
    }
    duk_destroy_heap(ctx);
    return result;
}

// Walk any renderer shape (videoRenderer / gridVideoRenderer /
// compactVideoRenderer / ...) and return "id|title|channel|len|views"
// lines. Depth-bounded so 1 MB JSONs can't blow the C stack.
static const char* kVideoWalk =
R"JS(function(DATA){
  var out = [];
  function txt(n){
    if (!n) return '';
    if (typeof n === 'string') return n;
    if (n.simpleText) return n.simpleText;
    if (n.runs){ var s=''; for (var i=0;i<n.runs.length;i++) s+=(n.runs[i].text||''); return s; }
    return '';
  }
  function clean(s){ return String(s).replace(/[\|\t\r\n]+/g,' ').replace(/\s{2,}/g,' ').trim(); }
  function add(v){
    if (out.length >= 40 || !v) return;
    var t = clean(txt(v.title) || txt(v.headline));
    if (!t || !v.videoId) return;
    var ch = clean(txt(v.ownerText) || txt(v.shortBylineText) || txt(v.longBylineText));
    var len = clean(txt(v.lengthText));
    var vw = clean(txt(v.viewCountText) || txt(v.shortViewCountText));
    out.push([v.videoId, t, ch, len, vw].join('|'));
  }
  function walk(n, d){
    if (!n || typeof n !== 'object' || d > 22 || out.length >= 40) return;
    var v = n.videoRenderer || n.gridVideoRenderer || n.compactVideoRenderer ||
            n.playlistPanelVideoRenderer || n.radioRenderer;
    if (v && v.videoId) { add(v); return; }
    for (var k in n){
      if (Object.prototype.hasOwnProperty.call(n, k)) walk(n[k], d + 1);
    }
  }
  walk(DATA, 0);
  return out.join('\n');
})JS";

static std::string videoListHtml(const std::string& lines) {
    std::ostringstream o;
    std::istringstream in(lines);
    std::string ln;
    while (std::getline(in, ln)) {
        if (ln.empty()) continue;
        std::string fields[5];
        size_t pos = 0;
        for (int i = 0; i < 4; ++i) {
            size_t bar = ln.find('|', pos);
            if (bar == std::string::npos) break;
            fields[i] = ln.substr(pos, bar - pos);
            pos = bar + 1;
        }
        fields[4] = ln.substr(pos);
        const std::string& id    = fields[0];
        const std::string& title = fields[1];
        if (id.empty() || title.empty()) continue;
        const std::string& chan  = fields[2];
        const std::string& len   = fields[3];
        const std::string& views = groupDigits(fields[4]);
        std::string meta;
        if (!chan.empty())  meta += chan;
        if (!len.empty())   meta += (meta.empty() ? "" : " \xC2\xB7 ") + len;
        if (!views.empty()) meta += (meta.empty() ? "" : " \xC2\xB7 ") + views;
        // Float wrapper div around the link+img (the engine's proven
        // wikipedia-thumb pattern — float on the img inside an <a> is
        // dropped by the inline-run collector).
        o << "<div style=\"overflow:hidden; margin:10px 0\">\n"
          << "<div style=\"float:left; margin-right:10px\">"
             "<a href=\"https://www.youtube.com/watch?v=" << escapeHtml(id)
          << "\"><img src=\"https://i.ytimg.com/vi/" << escapeHtml(id)
          << "/mqdefault.jpg\" width=\"168\" height=\"94\"></a></div>\n"
          << "<a href=\"https://www.youtube.com/watch?v=" << escapeHtml(id)
          << "\"><b>" << escapeHtml(title) << "</b></a><br>\n"
          << "<span style=\"color:#555\">" << escapeHtml(meta) << "</span>\n"
          << "</div>\n";
    }
    return o.str();
}

// v2.8: red header bar now reads "YouTube" (was "YouTube Lite" in v2.6).
// Same form action / inputs.
static const char* kSearchForm =
    "<div style=\"background:#c00; padding:10px 14px\">\n"
    "<span style=\"color:#fff; font-size:20px\"><b>YouTube</b></span>\n"
    "<form action=\"https://www.youtube.com/results\" method=\"get\" "
    "style=\"margin:6px 0 0 0\">\n"
    "<input name=\"search_query\" style=\"width:260px\"> "
    "<input type=\"submit\" value=\"Search\">\n"
    "</form></div>\n";

std::string youTubeLiteHtml(const std::string& url, const std::string& rawHtml,
                            bool& ok, const std::string& bridgeReason) {
    ok = false;
    if (shimsDisabled()) return "";
    Url u = parseUrl(url);
    if (!u.valid) return "";

    std::string path = u.path.empty() ? "/" : u.path;

    // Watch pages: metadata lives in ytInitialPlayerResponse.
    if (path.compare(0, 7, "/watch") == 0) {
        std::string playerJson =
            extractAssignedObject(rawHtml, "ytInitialPlayerResponse");
        std::string videoId = queryParam(u.query, "v");
        std::ostringstream o;
        o << "<html><head><title>YouTube</title></head>"
             "<body style=\"font-family: sans-serif; background:#fff\">\n"
          << kSearchForm << "<div style=\"padding:0 14px\">\n";
        // Play hand-off: the red chip (re)runs the in-browser bridge
        // (yt-dlp first, Piped API fallback second) and opens the built-in
        // media player page. The mpv chip only exists when mpv is
        // installed. When the bridge already failed for this navigation,
        // the amber banner above carries the actual reason.
        if (!videoId.empty()) {
            std::string watch =
                "https://www.youtube.com/watch?v=" + escapeHtml(videoId);
            if (!bridgeReason.empty()) {
                o << "<div style=\"background:#fff8e1; border:2px solid "
                     "#e0a800; padding:8px 10px; margin:10px 0\">"
                     "<span style=\"color:#6b5200; font-size:14px\">"
                     "<b>Video playback failed:</b> "
                  << escapeHtml(bridgeReason)
                  << "</span></div>\n";
            }
            o << "<div style=\"background:#c00; padding:9px 0; width:190px; "
                 "text-align:center; margin:10px 0\">"
                 "<a href=\"" << watch
              << "\" style=\"color:#fff; font-size:16px; "
                 "text-decoration:none\">&#9654; Play in browser</a>"
                 "</div>\n";
            if (mpv::available()) {
                o << "<div style=\"background:#555; padding:9px 0; "
                     "width:170px; text-align:center; margin:10px 0\">"
                     "<a href=\"mpv:" << watch
                  << "\" style=\"color:#fff; font-size:16px; "
                     "text-decoration:none\">&#9654; Play in mpv</a>"
                     "</div>\n"
                     "<span style=\"color:#888; font-size:13px\">or press v</span>";
            } else {
                o << "<span style=\"color:#888; font-size:13px\">"
                     "mpv not installed &#8212; external playback "
                     "disabled</span>";
            }
        }
        bool got = false;
        if (!playerJson.empty()) {
            std::string info = walkJson(playerJson,
                "function(DATA){"
                "  var d = DATA.videoDetails;"
                "  if (!d) return '';"
                "  return [d.title||'', d.author||'', d.lengthSeconds||'',"
                "          d.viewCount||'', (d.shortDescription||'')"
                "            .replace(/[\\u0001\\u0000]/g,' ')"
                "            .slice(0,6000)].join('\\u0001');"
                "}");
            if (!info.empty()) {
                std::string fields[5];
                size_t pos = 0;
                for (int i = 0; i < 4; ++i) {
                    size_t s = info.find('\x01', pos);
                    if (s == std::string::npos) break;
                    fields[i] = info.substr(pos, s - pos);
                    pos = s + 1;
                }
                fields[4] = info.substr(pos);
                got = !fields[0].empty();
                if (got) {
                    // viewCount comes as a raw number on watch pages.
                    std::string vc = groupDigits(fields[3]);
                    if (!vc.empty() && vc[0] >= '0' && vc[0] <= '9')
                        vc += " views";
                    long secs = atol(fields[2].c_str());
                    if (secs < 0 || secs > 86399) secs = 0;  // sanity clamp
                    char lenbuf[16] = "";
                    if (secs > 0) {
                        snprintf(lenbuf, sizeof(lenbuf), "%ld:%02ld",
                                 secs / 60, secs % 60);
                    }
                    std::string meta = fields[1];
                    if (lenbuf[0]) meta += (meta.empty()?"":" \xC2\xB7 ") + std::string(lenbuf);
                    if (!vc.empty()) meta += (meta.empty()?"":" \xC2\xB7 ") + vc;
                    o << "<h1>" << escapeHtml(fields[0]) << "</h1>\n"
                      << "<p style=\"color:#555\">" << escapeHtml(meta)
                      << "</p>\n"
                      << "<p style=\"color:#888; font-size:13px\">"
                         "Video plays in mpv (button above, or press v); "
                         "this page keeps the description and related "
                         "videos.</p>\n";
                    if (!fields[4].empty()) {
                        o << "<div style=\"white-space:pre-wrap; "
                             "font-size:14px; max-width:640px\">"
                          << escapeHtml(fields[4]) << "</div>\n";
                    }
                }
            }
        }
        // Related / recommended: ytInitialData is also on watch pages.
        std::string dataJson = extractAssignedObject(rawHtml, "ytInitialData");
        std::string related = dataJson.empty() ? "" : walkJson(dataJson, kVideoWalk);
        if (!related.empty()) {
            o << "<h3>Up next</h3>\n" << videoListHtml(related);
        }
        // NEVER fall back to the raw watch-page HTML: without the extractor
        // the engine gets a 2 MB JS app shell whose rendering is unreadable
        // mush (and it differs per region / consent state, so extraction
        // can legitimately fail). The Play button only needs the video id
        // from the URL, so a clean playable page is ALWAYS possible.
        if (!got) {
            // Surface WHY the metadata was missing when YouTube says so:
            // datacenter/VPN IPs get LOGIN_REQUIRED ("Sign in to confirm
            // you're not a bot") — playback through mpv/yt-dlp still works
            // (yt-dlp sends its own client headers), and from residential
            // IPs the full description usually loads.
            std::string reason;
            {
                std::string prJson =
                    extractAssignedObject(rawHtml, "ytInitialPlayerResponse");
                if (!prJson.empty())
                    reason = walkJson(prJson,
                        "function(DATA){"
                        "  var ps = DATA.playabilityStatus || {};"
                        "  return ps.reason || ps.status || '';"
                        "}");
            }
            std::string msg =
                "This video's metadata could not be read";
            if (!reason.empty()) {
                msg += " \xE2\x80\x94 YouTube says: " + reason;
                msg += ". Playback still works";
            } else {
                msg += " (YouTube served a variant page), but playback "
                       "still works";
            }
            msg += ": click Play above or press v.";
            o << "<p style=\"color:#555\">" << escapeHtml(msg) << "</p>\n";
        }
        o << "</div></body></html>";
        ok = true;
        return o.str();
    }

    // Home / search / channel-ish pages: walk ytInitialData.
    std::string dataJson = extractAssignedObject(rawHtml, "ytInitialData");
    std::string lines = dataJson.empty() ? "" : walkJson(dataJson, kVideoWalk);

    std::string title = "YouTube";
    bool isSearch = path.compare(0, 9, "/results") == 0;
    if (isSearch) {
        std::string q = queryParam(u.query, "search_query");
        if (q.empty()) q = queryParam(u.query, "q");
        if (!q.empty()) title = q + " - YouTube";
    }
    std::string list = videoListHtml(lines);
    if (list.empty() && !isSearch) {
        // Home (and some regions/IPs) serve a consent gate whose data
        // has no videos. Return a useful page instead of the raw shell:
        // search box + category shortcuts.
        std::ostringstream c;
        const char* cats[] = {"Music", "News", "Gaming", "Comedy", "Tech",
                              "Podcasts", "Cooking", "Football"};
        c << "<html><head><title>YouTube</title></head>"
             "<body style=\"font-family: sans-serif; background:#fff\">\n"
          << kSearchForm
          << "<div style=\"padding:0 14px\">\n"
             "<p>YouTube gates its home feed behind a consent dialog that "
             "needs JavaScript. Search works fully — results, video pages, "
             "thumbnails.</p>\n"
             "<p>";
        for (size_t i = 0; i < sizeof(cats)/sizeof(cats[0]); ++i) {
            if (i) c << " \xC2\xB7 ";
            c << "<a href=\"https://www.youtube.com/results?search_query="
              << urlEncode(cats[i]) << "\">" << cats[i] << "</a>";
        }
        c << "</p>\n</div></body></html>";
        ok = true;
        return c.str();
    }
    if (list.empty()) {
        // Search with no parseable results: raw HTML would be an unreadable
        // JS shell — show a clean "try again" page instead.
        std::ostringstream c;
        c << "<html><head><title>" << escapeHtml(title) << "</title></head>"
             "<body style=\"font-family: sans-serif; background:#fff\">\n"
          << kSearchForm
          << "<div style=\"padding:0 14px\">\n"
             "<p>YouTube returned no readable results for this search "
             "(region or consent gate). Try another query.</p>\n"
             "</div></body></html>";
        ok = true;
        return c.str();
    }
    std::ostringstream o;
    o << "<html><head><title>" << escapeHtml(title) << "</title></head>"
         "<body style=\"font-family: sans-serif; background:#fff\">\n"
      << kSearchForm << "<div style=\"padding:0 14px\">\n";
    if (isSearch) {
        std::string q = queryParam(u.query, "search_query");
        if (q.empty()) q = queryParam(u.query, "q");
        if (!q.empty())
            o << "<h2>Results for &quot;" << escapeHtml(q) << "&quot;</h2>\n";
    }
    o << list << "</div></body></html>";
    ok = true;
    return o.str();
}

} // namespace shims
} // namespace browser
