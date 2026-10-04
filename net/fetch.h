#pragma once
#include <SDL2/SDL.h>
#include <string>

namespace browser {

// Result of one HTTP(S) fetch.
struct FetchResult {
    bool        ok = false;        // transport succeeded AND HTTP status 2xx/3xx
    long        status = 0;        // HTTP status code (0 on transport error)
    std::string body;              // raw response body (already decompressed)
    std::string contentType;       // mime type, lowercase, without parameters
    std::string finalUrl;          // URL after following redirects
    std::string error;             // human-readable transport error, if !ok
};

// Fetch `url` over HTTP(S) using libcurl. Features:
//   - HTTPS via OpenSSL (system CA store)
//   - transparent gzip/deflate/brotli decompression (Accept-Encoding)
//   - follows up to 10 redirects
//   - configurable timeout, default 25s (connect timeout 10s)
FetchResult fetchUrl(const std::string& url, long timeoutSec = 25);

// Same as fetchUrl but backed by a per-session memory cache so a page's
// stylesheets/scripts/images aren't re-downloaded for every reference.
// Cache lives for the process lifetime; call clearFetchCache() on reload
// if fresh data is wanted.
FetchResult fetchUrlCached(const std::string& url, long timeoutSec = 25);
void clearFetchCache();

// Release this thread's pooled curl easy handle (and its connection
// cache). Short-lived worker threads call this just before exiting so
// handles don't leak; long-lived pool threads never need to.
void releaseThreadCurl();

// Decode an in-memory image (PNG/JPEG/GIF/BMP/WEBP/SVG/...) into an
// SDL_Surface. Returns nullptr on failure. Caller must NOT free the surface
// directly; it is owned by the caller like any IMG_Load result.
SDL_Surface* decodeImageBytes(const std::string& bytes, const std::string& hintUrl = "");

// Parse a "data:[mime][;base64],payload" URI. Returns false when `uri`
// isn't a data URI or the payload is empty. base64 and percent-encoded
// payloads are both supported.
bool parseDataUri(const std::string& uri, std::string& outBytes,
                  std::string& outMime);

// Decode a data: URI directly into an SDL_Surface (nullptr on failure).
SDL_Surface* decodeDataUriImage(const std::string& uri, const std::string& hintUrl = "");

} // namespace browser
