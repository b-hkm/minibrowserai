// yt-dlp bridge: turn page URLs (YouTube watch/shorts/youtu.be/embeds)
// into direct progressive media URLs the internal FFmpeg player can play.
//
// Two extraction paths, tried in order:
//
//   1. yt-dlp (or youtube-dl) subprocess — the same resolver mpv's
//      ytdl_hook uses. Best quality/metadata; needs the tool installed
//      and a network YouTube does not bot-gate.
//   2. Piped API fallback (v2.6) — community extraction servers. The
//      instance resolves the video and proxies the bytes, so it works
//      on bot-gated networks AND needs no local tooling. Streams top
//      out ~360p and instances come and go (list below / env override).
//
// Why any of this is needed: YouTube does not serve a plain video file.
// Its player JS assembles DASH/HLS segments through MediaSource
// Extensions, which a minimal one-input player cannot do. Both paths
// yield a progressive stream URL (video+audio in one file, plain
// http(s) download) the browser plays on the built-in media page —
// everything in-process: no mpv, no external window.
//
// Env knobs:
//   MINIBROWSER_YTDLP_ARGS   extra yt-dlp args (cookies, clients, proxy)
//   MINIBROWSER_PIPED_HOSTS  comma-separated Piped API hosts overriding
//                            the built-in list, e.g.
//                            "pipedapi.ducks.party,my-own.host/api"
//
// Threading: resolve() is BLOCKING (process spawn + network, typically
// 1-10 s) and thread-safe; call it from worker threads (the Browser
// resolves on a detached thread and polls the result on the UI thread).
#pragma once

#include <string>
#include <vector>

namespace browser {
namespace media {
namespace extractor {

// Extraction result. directUrl is a progressive http(s) media URL ready
// for MediaPlayer::open().
struct ResolvedMedia {
    std::string directUrl;   // progressive stream URL
    std::string title;       // video title ("" when the tool cannot tell)
    std::string ext;         // container ("mp4", "webm", ...)
    std::string via;         // "yt-dlp", "youtube-dl", "piped:<host>"
                             // or "cache"
};

// True for watch-style YouTube URLs the bridge should handle: /watch,
// /shorts/, /embed/, /live/ on youtube.com (any common host variant,
// incl. music.youtube.com and youtube-nocookie.com) and youtu.be/<id>
// short links. Home/search/channel pages are NOT extractable.
bool isExtractableUrl(const std::string& url);

// Resolver on PATH: "yt-dlp", "youtube-dl", or "" (none). Cached after
// the first call (like mpv::available() — install later, restart later).
std::string whichTool();
bool available();            // !whichTool().empty()

// Full resolve: yt-dlp subprocess first, Piped API second, unless a
// fresh result (2 h TTL) is cached. Blocking. On failure `err` carries
// a human-readable reason covering BOTH paths (bot gate, unavailable
// video, missing tool...).
bool resolve(const std::string& pageUrl, ResolvedMedia& out,
             std::string& err);

// Video id of a watch-style URL ("" when there is none). Handles
// ?v=, /shorts/, /embed/, /live/, /v/ and youtu.be short links.
std::string videoId(const std::string& pageUrl);

// Piped API hosts, in try order. MINIBROWSER_PIPED_HOSTS overrides the
// built-in list (comma/semicolon/space separated).
std::vector<std::string> pipedHosts();

// Parse a Piped /streams/<id> JSON body and pick the best playable
// progressive stream (videoOnly==false, non-HLS; MPEG_4 proxied preferred
// over LBRY/WEBM, then by parsed quality). Exposed for the selftest.
bool selectPipedStream(const std::string& json, ResolvedMedia& out,
                       std::string& err);

// Cache-only lookup (no process, no network). Used by the UI thread for
// instant back/forward to already-resolved videos.
bool peek(const std::string& pageUrl, ResolvedMedia& out);

// Cache introspection (tests / status line).
void  clearCache();
size_t cacheSize();

// POSIX-shell single-argument quoting ('...'\''...' style). Exposed so
// tests can prove URLs containing quotes/metacharacters survive intact.
std::string shellQuote(const std::string& s);

// Diagnostic log for the resolver's stderr (TMPDIR/mini-browser-ytdlp.log).
std::string logPath();

} // namespace extractor
} // namespace media
} // namespace browser
