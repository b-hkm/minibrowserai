#include "extractor.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unistd.h>
#include <unordered_map>
#include <vector>

#include "minjson.h"
#include "../net/fetch.h"
#include "../net/url.h"

using browser::media::minjson::Doc;
using browser::media::minjson::Val;

namespace browser {
namespace media {
namespace extractor {

// ---------------------------------------------------------------------------
// PATH lookup (same policy as app/mpv.cpp: absolute path or "").
// ---------------------------------------------------------------------------

static bool executable(const std::string& p) {
    return ::access(p.c_str(), X_OK) == 0;
}

static std::string findInPath(const std::string& name) {
    const char* pathEnv = ::getenv("PATH");
    if (!pathEnv || pathEnv[0] == '\0') return "";
    std::string spath = pathEnv;
    size_t start = 0;
    while (start <= spath.size()) {
        size_t colon = spath.find(':', start);
        std::string dir = spath.substr(
            start, colon == std::string::npos ? std::string::npos
                                              : colon - start);
        if (dir.empty()) dir = ".";
        while (!dir.empty() && dir.back() == '/') dir.pop_back();
        std::string cand = dir + "/" + name;
        if (executable(cand)) return cand;
        if (colon == std::string::npos) break;
        start = colon + 1;
    }
    return "";
}

std::string whichTool() {
    static const std::string cached = [] {
        if (!findInPath("yt-dlp").empty()) return std::string("yt-dlp");
        if (!findInPath("youtube-dl").empty()) return std::string("youtube-dl");
        return std::string("");
    }();
    return cached;
}

bool available() { return !whichTool().empty(); }

std::string logPath() {
    const char* tmp = ::getenv("TMPDIR");
    return std::string(tmp && tmp[0] ? tmp : "/tmp") +
           "/mini-browser-ytdlp.log";
}

// ---------------------------------------------------------------------------
// URL classification
// ---------------------------------------------------------------------------

// ?a=b&v=ID scanner: split on '&' then '='. Video ids never need
// percent-decoding ([A-Za-z0-9_-] only), so this stays byte-simple.
static std::string queryParam(const std::string& query, const char* key) {
    size_t i = 0;
    const size_t klen = std::strlen(key);
    while (i <= query.size()) {
        size_t amp = query.find('&', i);
        std::string pair = query.substr(
            i, amp == std::string::npos ? std::string::npos : amp - i);
        size_t eq = pair.find('=');
        if (eq != std::string::npos && pair.size() - eq - 1 > 0 &&
            pair.compare(0, eq, key) == 0 && eq == klen)
            return pair.substr(eq + 1);
        if (amp == std::string::npos) break;
        i = amp + 1;
    }
    return "";
}

static bool hostIs(const std::string& host, const char* domain) {
    if (host == domain) return true;
    std::string dot = std::string(".") + domain;
    return host.size() > dot.size() &&
           host.compare(host.size() - dot.size(), dot.size(), dot) == 0;
}

static bool isYouTubeHost(const std::string& host) {
    return hostIs(host, "youtube.com") || hostIs(host, "youtu.be") ||
           hostIs(host, "youtube-nocookie.com");
}

bool isExtractableUrl(const std::string& url) {
    if (url.empty()) return false;
    // The explicit external-player escape hatch is never auto-extracted.
    if (url.compare(0, 4, "mpv:") == 0) return false;
    if (url.compare(0, 12, "view-source:") == 0) return false;

    Url u = parseUrl(url);
    if (!u.valid) return false;
    if (u.scheme != "http" && u.scheme != "https") return false;
    if (!isYouTubeHost(u.host)) return false;

    // youtu.be short links carry the id as the whole path.
    if (hostIs(u.host, "youtu.be")) return u.path.size() > 1;

    // youtube.com family: a specific video, not home/search/channel.
    return u.path.find("/watch") == 0 ||
           u.path.find("/shorts/") == 0 ||
           u.path.find("/embed/") == 0 ||
           u.path.find("/live/") == 0 ||
           u.path.find("/v/") == 0;
}

// ---------------------------------------------------------------------------
// Shell quoting
// ---------------------------------------------------------------------------

std::string shellQuote(const std::string& s) {
    if (s.empty()) return "''";
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else           out += c;
    }
    out += "'";
    return out;
}

// ---------------------------------------------------------------------------
// Result cache: googlevideo stream URLs stay valid for hours; repeated
// resolves (back/forward, reload) must be instant and free.
// ---------------------------------------------------------------------------

namespace {

struct CacheEntry {
    ResolvedMedia m;
    std::chrono::steady_clock::time_point at;
};

constexpr double kTtlSeconds = 2.0 * 60 * 60;  // googlevideo lives ~6 h
constexpr size_t kMaxEntries = 64;

std::mutex g_cacheM;
std::unordered_map<std::string, CacheEntry> g_cache;

double ageSeconds(const CacheEntry& e) {
    return std::chrono::duration<double>(
               std::chrono::steady_clock::now() - e.at)
        .count();
}

void pruneLocked() {
    for (auto it = g_cache.begin(); it != g_cache.end();) {
        if (ageSeconds(it->second) > kTtlSeconds)
            it = g_cache.erase(it);
        else
            ++it;
    }
    while (g_cache.size() > kMaxEntries)          // rough LRU-ish cap
        g_cache.erase(g_cache.begin());
}

std::string trimLine(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' ||
                          s.back() == ' '  || s.back() == '\t'))
        s.pop_back();
    size_t a = s.find_first_not_of(" \t");
    return a == std::string::npos ? "" : s.substr(a);
}

// Last non-empty line of the resolver log, single-lined and capped —
// surfaces WHY an extraction failed (bot check, age gate, bad URL...).
std::string logTail() {
    FILE* f = std::fopen(logPath().c_str(), "r");
    if (!f) return "";
    std::string last;
    char buf[2048];
    while (std::fgets(buf, sizeof buf, f)) {
        std::string l = trimLine(buf);
        if (!l.empty()) last = l;
    }
    std::fclose(f);
    for (char& c : last)
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    if (last.size() > 300) last = last.substr(0, 300) + "...";
    return last;
}

// Reads all stdout lines from the resolver (popen), tolerating long ones.
std::vector<std::string> readAllLines(FILE* p) {
    std::vector<std::string> lines;
    std::string cur;
    char buf[4096];
    size_t got;
    while ((got = std::fread(buf, 1, sizeof buf, p)) > 0) {
        for (size_t i = 0; i < got; ++i) {
            if (buf[i] == '\n') {
                lines.push_back(cur);
                cur.clear();
                if (lines.size() > 64) return lines;   // paranoia cap
            } else if (buf[i] != '\r') {
                if (cur.size() < 16384) cur += buf[i];
            }
        }
    }
    if (!cur.empty()) lines.push_back(cur);
    return lines;
}

} // namespace

void clearCache() {
    std::lock_guard<std::mutex> lk(g_cacheM);
    g_cache.clear();
}

size_t cacheSize() {
    std::lock_guard<std::mutex> lk(g_cacheM);
    return g_cache.size();
}

bool peek(const std::string& pageUrl, ResolvedMedia& out) {
    std::lock_guard<std::mutex> lk(g_cacheM);
    auto it = g_cache.find(pageUrl);
    if (it == g_cache.end()) return false;
    if (ageSeconds(it->second) > kTtlSeconds) {
        g_cache.erase(it);
        return false;
    }
    out = it->second.m;
    out.via = "cache";
    return true;
}

// ---------------------------------------------------------------------------
// Video id extraction (shared by the Piped fallback)
// ---------------------------------------------------------------------------

static bool idChar(char c) {
    return std::isalnum((unsigned char)c) || c == '_' || c == '-';
}

// Grab the [A-Za-z0-9_-]{5,16} run starting at `s` ("" when too short).
static std::string idRun(const char* s) {
    std::string id;
    while (idChar(*s) && id.size() < 16) id += *s++;
    return id.size() >= 5 ? id : "";
}

std::string videoId(const std::string& pageUrl) {
    Url u = parseUrl(pageUrl);
    if (!u.valid) return "";
    if (hostIs(u.host, "youtu.be")) {
        // Path is "/<id>" (possibly "/<id>/" with junk after).
        if (u.path.size() > 1) return idRun(u.path.c_str() + 1);
        return "";
    }
    if (!isYouTubeHost(u.host)) return "";
    if (u.path.compare(0, 7, "/watch") == 0)
        return idRun(queryParam(u.query, "v").c_str());   // queryParam in url.h
    static const char* kPathPrefix[] = {"/shorts/", "/embed/", "/live/",
                                        "/v/"};
    for (const char* pre : kPathPrefix) {
        size_t p = u.path.find(pre);
        if (p != std::string::npos)
            return idRun(u.path.c_str() + p + std::strlen(pre));
    }
    return "";
}

// ---------------------------------------------------------------------------
// Piped fallback: community API servers that hand out progressive stream
// URLs (proxied through the instance's own video host, so it works even
// when YouTube bot-gates OUR network — the instance does the extraction).
// ---------------------------------------------------------------------------

std::vector<std::string> pipedHosts() {
    std::vector<std::string> hosts;
    if (const char* env = ::getenv("MINIBROWSER_PIPED_HOSTS")) {
        // Power-user override: comma/space/semicolon separated.
        std::string s = env;
        size_t i = 0;
        while (i < s.size()) {
            size_t j = i;
            while (j < s.size() && s[j] != ',' && s[j] != ';' && s[j] != ' ')
                ++j;
            std::string h = s.substr(i, j - i);
            while (!h.empty() && h.back() == '/') h.pop_back();
            if (!h.empty()) hosts.push_back(h);
            i = (j < s.size()) ? j + 1 : j;
        }
        if (!hosts.empty()) return hosts;
    }
    // Defaults: instances verified live (2026-10); API enabled, no
    // Cloudflare interstitial on /streams. Order = try order.
    hosts.push_back("pipedapi.ducks.party");
    hosts.push_back("api.piped.private.coffee");
    return hosts;
}

namespace {

int qualityScore(const std::string& q) {
    // "720p" -> 720, "1080p60" -> 1080, "LBRY"/"" -> 0.
    int v = 0;
    for (char c : q) {
        if (std::isdigit((unsigned char)c)) v = v * 10 + (c - '0');
        else if (v) break;
    }
    return v;
}

int formatScore(const std::string& fmt) {
    // MPEG_4  = proxied YouTube progressive mp4 (range requests, has
    //           audio) — first choice.
    // MP4     = LBRY mirror (no YouTube involvement; signed URLs, works
    //           when it works).
    // WEBM    = rare progressive webm.
    if (fmt == "MPEG_4") return 3;
    if (fmt == "MP4")    return 2;
    if (fmt == "WEBM")   return 2;
    return 0;                        // HLS/dash-ish: unusable for us
}

const char* extForFormat(const std::string& fmt) {
    if (fmt == "WEBM") return "webm";
    return "mp4";
}

} // namespace

// Pick the best progressive (video+audio, non-HLS) stream from a Piped
// /streams/<id> JSON body. Exposed for the selftest (no network).
bool selectPipedStream(const std::string& json, ResolvedMedia& out,
                       std::string& err) {
    err.clear();
    Doc d;
    if (!d.parse(json)) {
        err = "invalid JSON from Piped";
        return false;
    }
    if (d["error"].isStr()) {
        // e.g. ContentNotAvailableException — YouTube said no to the
        // instance too: the video itself is gone, not our network.
        err = std::string(d["error"].str());
        if (err.size() > 200) err = err.substr(0, 200);
        return false;
    }
    const Val vs = d["videoStreams"];
    if (!vs.isArr()) {
        err = "Piped reply has no videoStreams";
        return false;
    }
    int bestIdx = -1, bestFmt = -1, bestQ = -1;
    for (unsigned i = 0; i < vs.arrSize(); ++i) {
        Val s = vs.at(i);
        if (!s["url"].isStr()) continue;
        const char* url = s["url"].str();
        if (std::strncmp(url, "http", 4) != 0) continue;
        if (s["videoOnly"].isBool() && s["videoOnly"].boolean())
            continue;                    // DASH video-only track
        std::string fmt = s["format"].str();
        int fs = formatScore(fmt);
        if (fs <= 0) continue;           // HLS etc. — not playable here
        int qs = qualityScore(s["quality"].str());
        if (fs > bestFmt || (fs == bestFmt && qs > bestQ)) {
            bestIdx = (int)i;            // index, not &s (loop-local!)
            bestFmt = fs;
            bestQ = qs;
        }
    }
    if (bestIdx < 0) {
        err = "no progressive stream in Piped reply";
        return false;
    }
    const Val b = vs.at((unsigned)bestIdx);
    out.directUrl = b["url"].str();
    out.title = d["title"].str();
    out.ext = extForFormat(b["format"].str());
    out.via = "piped";
    return true;
}

// Try every Piped host until one resolves the video. Blocking.
static bool resolveViaPiped(const std::string& vid, ResolvedMedia& out,
                            std::string& err) {
    err.clear();
    if (vid.empty()) {
        err = "no video id in URL";
        return false;
    }
    std::vector<std::string> tried;
    for (const std::string& host : pipedHosts()) {
        FetchResult fr = fetchUrl("https://" + host + "/streams/" + vid, 15);
        if (!fr.ok || fr.body.empty()) {
            tried.push_back(host + (fr.ok ? " (empty reply)"
                                          : " (" + fr.error.substr(0, 60) + ")"));
            continue;
        }
        ResolvedMedia rm;
        std::string serr;
        if (selectPipedStream(fr.body, rm, serr)) {
            rm.via = "piped:" + host;
            out = rm;
            return true;
        }
        tried.push_back(host + ": " + serr.substr(0, 120));
    }
    err = "Piped fallback failed";
    if (!tried.empty()) {
        err += " (";
        for (size_t i = 0; i < tried.size(); ++i) {
            if (i) err += "; ";
            err += tried[i];
        }
        err += ")";
    }
    return false;
}

// yt-dlp / youtube-dl subprocess: build the command, run it, parse
// url/title/ext lines. Does NOT touch the cache (resolve() owns that).
static bool tryYtDlp_(const std::string& pageUrl, ResolvedMedia& out,
                      std::string& err);

// ---------------------------------------------------------------------------
// resolve(): yt-dlp first, Piped API second; cache successes.
// ---------------------------------------------------------------------------

bool resolve(const std::string& pageUrl, ResolvedMedia& out,
             std::string& err) {
    err.clear();
    if (!isExtractableUrl(pageUrl)) {
        err = "not a YouTube watch-style URL";
        return false;
    }
    if (peek(pageUrl, out)) return true;

    std::string ytdlpErr;
    bool ytdlpTried = false;

    std::string tool = whichTool();
    if (!tool.empty()) ytdlpTried = tryYtDlp_(pageUrl, out, ytdlpErr);

    if (ytdlpTried) {
        std::lock_guard<std::mutex> lk(g_cacheM);
        pruneLocked();
        g_cache[pageUrl] = CacheEntry{out, std::chrono::steady_clock::now()};
        return true;
    }

    // yt-dlp failed (or is not installed): Piped is the safety net. It
    // needs no local tooling at all and its streams are proxied through
    // the instance, so a bot-gated local network does not matter.
    ResolvedMedia pr;
    std::string pipedErr;
    if (resolveViaPiped(videoId(pageUrl), pr, pipedErr)) {
        std::lock_guard<std::mutex> lk(g_cacheM);
        pruneLocked();
        g_cache[pageUrl] = CacheEntry{pr, std::chrono::steady_clock::now()};
        out = pr;
        return true;
    }

    // Both paths failed: compose one honest, actionable reason.
    std::string msg;
    auto mentions = [](const std::string& s, std::initializer_list<const char*> ws) {
        for (const char* w : ws)
            if (s.find(w) != std::string::npos) return true;
        return false;
    };
    // v2.10: distinguish three failure modes:
    //   1. Video genuinely unavailable (deleted/private/region-locked/
    //      age-restricted). YouTube's extractor returns
    //      ContentNotAvailableException with "unavailable" / "private" /
    //      "removed" / "geo-block" in the message. Nothing we can do.
    //   2. Piped got bot-flagged by YouTube (YouTube now requires sign-in
    //      for anonymous watch access on the NewPipeExtractor client that
    //      Piped uses — affects ALL Piped instances globally, not just
    //      specific IPs). The error contains "SignInConfirm" /
    //      "LOGIN_REQUIRED" / "Sign in to confirm" / "anonymous watch".
    //      Fixable by installing yt-dlp (rotating clients, cookie support
    //      usually bypass this).
    //   3. yt-dlp itself got bot-flagged (same root cause, different
    //      code path). Cookies usually bypass this.
    const bool unavailable =
        mentions(pipedErr, {"unavailable", "not exist", "private", "removed",
                            "geo-block", "ContentNotAvailable"}) ||
        mentions(ytdlpErr, {"unavailable", "not exist", "private"});
    // Bot-gate signals. Note: must check BOTH errors because Piped's
    // NewPipeExtractor reports its own bot-gating (yt-dlp may not be
    // installed, in which case ytdlpErr is empty).
    const bool botGated =
        mentions(ytdlpErr, {"Sign in to confirm", "bot", "cookies"}) ||
        mentions(pipedErr, {"SignInConfirm", "LOGIN_REQUIRED",
                            "Sign in to confirm", "anonymous watch access"});

    if (unavailable) {
        msg = "video unavailable on YouTube (removed, private, "
              "age-restricted or region-locked) — confirmed by both "
              "yt-dlp and Piped";
    } else if (botGated) {
        // v2.10: this is the common failure mode in 2026 — YouTube
        // started requiring sign-in for anonymous watch access via the
        // NewPipeExtractor client (used by ALL Piped instances). The
        // video itself is fine; Piped just can't fetch its stream.
        // yt-dlp with rotating clients + cookies usually bypasses.
        if (tool.empty()) {
            msg = "YouTube is bot-gating Piped: " + pipedErr +
                  ". The video itself is fine — Piped's IP got "
                  "flagged. Install yt-dlp (pip install yt-dlp) — its "
                  "rotating clients and cookie support usually bypass "
                  "this; or set MINIBROWSER_YTDLP_ARGS="
                  "'--cookies-from-browser firefox' to reuse your "
                  "browser's YouTube session";
        } else {
            msg = "YouTube is bot-gating: yt-dlp reports '" + ytdlpErr +
                  "', Piped reports '" + pipedErr + "'. Cookies usually "
                  "bypass this: MINIBROWSER_YTDLP_ARGS="
                  "'--cookies-from-browser firefox'";
        }
    } else if (tool.empty()) {
        msg = "yt-dlp not installed and Piped failed: " + pipedErr +
              ". Install yt-dlp (pip install yt-dlp) for the full extractor";
    } else {
        msg = tool + ": " + ytdlpErr + " \xC2\xB7 " + pipedErr;
    }
    err = msg.substr(0, 500);
    return false;
}

static bool tryYtDlp_(const std::string& pageUrl, ResolvedMedia& out,
                      std::string& err) {
    const std::string tool = whichTool();

    // Format selector: best PROGRESSIVE format (video AND audio in one
    // file, plain http(s) download). `b` = best combined format; the
    // protocol filter keeps HLS/DASH manifests out — our one-input player
    // cannot merge separate audio/video tracks.
    std::string cmd;
    if (!findInPath("timeout").empty()) cmd += "timeout 75 ";
    cmd += tool;
    cmd += " --no-playlist --no-warnings --socket-timeout 15";
    // v2.11: format selector now explicitly requires BOTH audio and
    // video codecs (acodec!=none AND vcodec!=none) AND prefers the
    // https protocol (progressive combined formats). The v2.10
    // selector `b[protocol^=http]/b` was matching `http_dash_segments`
    // too — a DASH protocol that returns video-only OR audio-only
    // streams, not progressive combined files. For YouTube in 2026,
    // most videos only have adaptive (DASH) streams, so the v2.10
    // selector would fall through to `b` and pick a video-only DASH
    // stream — the file would have video but no audio, which is why
    // the user heard "buzz sound, not the actual content of the
    // video" (the audio track was missing, the buzz was the SDL audio
    // device playing silence/garbage).
    // v2.11 selector priorities (most-preferred first):
    //   1. best progressive https combined (audio+video) — what we want
    //   2. best progressive combined (audio+video, any protocol)
    //   3. best combined (audio+video, any protocol, including DASH
    //      adaptive with both codecs in one stream — rare but exists)
    //   4. fallback: best anything (last resort — might be video-only)
    cmd += " -f " + shellQuote(
        "best[protocol=https][acodec!=none][vcodec!=none]"
        "/best[acodec!=none][vcodec!=none]"
        "/best[acodec!=none]"
        "/best");
    // v2.10: explicit player_client order. yt-dlp's default client list
    // includes the "web" client which YouTube now bot-gates aggressively.
    // Forcing the "android" client first (which uses a different YouTube
    // API path that's still bot-tolerant) and the "tv_embedded" client
    // second (which is age-gate bypassable) usually gets a stream when
    // the default fails. Only applies to yt-dlp (youtube-dl doesn't
    // support --extractor-args).
    if (tool == "yt-dlp") {
        cmd += " --extractor-args " +
               shellQuote("youtube:player_client=android,tv_embedded,web");
    }
    // v2.10: try to reuse the user's browser cookies by default. yt-dlp
    // supports --cookies-from-browser firefox/chrome/brave/edge/chromium
    // — if a known browser is installed, use it. This reuses the user's
    // signed-in YouTube session, which bypasses YouTube's "Sign in to
    // confirm you're not a bot" wall. The user can override via
    // MINIBROWSER_YTDLP_ARGS= (including setting it to empty).
    // Power-user escape hatch (cookies, player clients, proxies...).
    bool userOverride = false;
    if (const char* extra = ::getenv("MINIBROWSER_YTDLP_ARGS")) {
        std::string e = trimLine(extra);
        if (!e.empty()) {
            cmd += " " + e;
            userOverride = true;
        }
    }
    if (!userOverride && tool == "yt-dlp") {
        // Auto-detect a browser with a YouTube session. Check firefox
        // first (most common on Linux), then chrome, then brave, then
        // chromium. The first one with a cookies.sqlite in the standard
        // location wins.
        const char* home = ::getenv("HOME");
        if (home && *home) {
            const std::string profiles[] = {
                std::string(home) + "/.mozilla/firefox",
                std::string(home) + "/.config/google-chrome",
                std::string(home) + "/.config/brave",
                std::string(home) + "/.config/chromium",
                std::string(home) + "/.config/microsoft-edge"
            };
            const char* browsers[] = {"firefox", "chrome", "brave",
                                       "chromium", "edge"};
            for (size_t i = 0; i < sizeof(profiles)/sizeof(profiles[0]); ++i) {
                std::string cmd2 = "test -d " + shellQuote(profiles[i]) +
                                   " && ls " + shellQuote(profiles[i]) +
                                   " >/dev/null 2>&1";
                if (std::system(cmd2.c_str()) == 0) {
                    cmd += std::string(" --cookies-from-browser ") + browsers[i];
                    break;
                }
            }
        }
    }
    if (tool == "yt-dlp") {
        // One value per line: url, title, ext.
        cmd += " --print " + shellQuote("%(url)s");
        cmd += " --print " + shellQuote("%(title)s");
        cmd += " --print " + shellQuote("%(ext)s");
    } else {
        cmd += " --get-url";                       // youtube-dl: url only
    }
    // stderr -> diagnostic log; stdout stays parseable.
    cmd += " " + shellQuote(pageUrl) + " 2>" + shellQuote(logPath());

    FILE* p = ::popen(cmd.c_str(), "r");
    if (!p) {
        err = "cannot run " + tool;
        return false;
    }
    std::vector<std::string> lines = readAllLines(p);
    int rc = ::pclose(p);

    // Parse: first line must look like a URL.
    ResolvedMedia rm;
    for (const std::string& l : lines) {
        if (l.compare(0, 7, "http://") == 0 ||
            l.compare(0, 8, "https://") == 0) {
            rm.directUrl = l;
            break;
        }
    }
    if (!rm.directUrl.empty() && tool == "yt-dlp") {
        // yt-dlp printed url/title/ext in that order.
        auto it = std::find(lines.begin(), lines.end(), rm.directUrl);
        if (it != lines.end()) {
            auto next = it + 1;
            if (next != lines.end() && next->compare(0, 7, "http://") != 0 &&
                next->compare(0, 8, "https://") != 0) {
                rm.title = *next++;
                if (next != lines.end() && !next->empty() &&
                    next->size() < 16 && next->find('/') == std::string::npos)
                    rm.ext = *next;
            }
        }
    }

    if (rm.directUrl.empty()) {
        std::string tail = logTail();
        err = tool + (rc == 0 ? " returned no URL"
                              : " failed (exit " + std::to_string(rc >> 8) + ")");
        if (!tail.empty()) err += ": " + tail;
        if (tail.find("Sign in to confirm") != std::string::npos ||
            tail.find("bot") != std::string::npos ||
            tail.find("cookies") != std::string::npos)
            err += " - YouTube is bot-checking this network; cookies help: "
                   "MINIBROWSER_YTDLP_ARGS='--cookies-from-browser firefox'";
        return false;
    }

    rm.via = tool;
    out = rm;
    return true;
}

} // namespace extractor
} // namespace media
} // namespace browser
