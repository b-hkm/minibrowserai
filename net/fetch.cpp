#include "fetch.h"
#include <curl/curl.h>
#include <SDL2/SDL_image.h>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <iostream>
#include <mutex>
#include <unordered_map>

namespace browser {

// ---------------------------------------------------------------------------
// libcurl plumbing
// ---------------------------------------------------------------------------

// curl_global_init is not thread-safe: with the parallel image preloader,
// several threads can call curl_easy_init simultaneously on a fresh
// process. Initialize the library once, on whatever thread gets there
// first, before any easy handle is created.
static std::once_flag g_curlOnce;
static void ensureCurlInit() {
    std::call_once(g_curlOnce,
                   []() { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

static size_t writeToString(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

// Case-insensitive find of a header name; returns value or "".
static std::string headerValue(const std::string& headers, const std::string& name) {
    std::string lowerHeaders = headers;
    std::string lowerName = name;
    std::transform(lowerHeaders.begin(), lowerHeaders.end(), lowerHeaders.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    size_t p = lowerHeaders.find(lowerName + ":");
    if (p == std::string::npos) return "";
    p += lowerName.size() + 1;
    size_t eol = lowerHeaders.find('\n', p);
    if (eol == std::string::npos) eol = lowerHeaders.size();
    std::string v = headers.substr(p, eol - p);
    // trim
    size_t a = v.find_first_not_of(" \t\r");
    if (a == std::string::npos) return "";
    size_t b = v.find_last_not_of(" \t\r");
    return v.substr(a, b - a + 1);
}

// ---------------------------------------------------------------------------
// Per-thread easy-handle reuse
//
// The naive path created a fresh curl handle per request, which forces a
// brand-new TCP + TLS handshake for EVERY transfer. A gallery page with 50
// images from one CDN paid 50 handshakes (~100-300 ms each on weak CPU);
// reusing one handle per thread keeps the connection cache (and the TLS
// session) warm, so transfers after the first on a host are keep-alive
// round trips. curl_easy_reset between uses clears all request state while
// keeping the connection pool intact.
// ---------------------------------------------------------------------------
static thread_local CURL* t_easy = nullptr;

static CURL* acquireCurl() {
    if (!t_easy) {
        t_easy = curl_easy_init();
    } else {
        curl_easy_reset(t_easy);
    }
    return t_easy;
}

// Called by short-lived worker threads just before they exit so the handle
// and its connection cache are released promptly. Long-lived threads (the
// image pool) simply never call this.
void releaseThreadCurl() {
    if (t_easy) {
        curl_easy_cleanup(t_easy);
        t_easy = nullptr;
    }
}

// Case-insensitive host check with subdomain match ("www.youtube.com"
// matches "youtube.com").
static bool hostEndsHost(const std::string& url, const char* domain) {
    // extract host between "://" and first '/'/?/#
    size_t scheme = url.find("://");
    size_t hStart = (scheme == std::string::npos) ? 0 : scheme + 3;
    size_t hEnd = url.find_first_of("/?#!", hStart);
    if (hEnd == std::string::npos) hEnd = url.size();
    std::string host = url.substr(hStart, hEnd - hStart);
    // strip userinfo/port
    size_t at = host.rfind('@');
    if (at != std::string::npos) host = host.substr(at + 1);
    size_t colon = host.find(':');
    if (colon != std::string::npos) host = host.substr(0, colon);
    std::transform(host.begin(), host.end(), host.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    std::string d = domain;
    if (host == d) return true;
    std::string dot = "." + d;
    return host.size() > dot.size() &&
           host.compare(host.size() - dot.size(), dot.size(), dot) == 0;
}

static bool isYouTubeHost(const std::string& url) {
    return hostEndsHost(url, "youtube.com") ||
           hostEndsHost(url, "youtu.be") ||
           hostEndsHost(url, "youtube-nocookie.com");
}

// v2.9: Cookie jar — persists YouTube's VISITOR_INFO1_LIVE / NID and the
// SOCS/CONSENT/PREF consent cookies across requests AND across process
// restarts. Without this, every fetch looks like a brand-new session that
// hasn't accepted the consent wall, and YouTube re-serves the consent
// interstitial on every visit. With the jar, the first visit sets the
// session cookies and subsequent visits skip the wall — exactly what a
// real Chrome session does.
static const char* cookieJarPath() {
    static const char* p = [] {
        const char* home = getenv("HOME");
        if (!home || !*home) home = "/tmp";
        static std::string path;
        path = std::string(home) + "/.cache/minibrowser/cookies.txt";
        std::string cmd = "mkdir -p " + std::string(home) +
                          "/.cache/minibrowser >/dev/null 2>&1";
        if (std::system(cmd.c_str()) == 0) {}
        return path.c_str();
    }();
    return p;
}

// v2.9: optional Referer header — set by the caller before fetchUrl() to
// the URL of the page the user is currently on (Chrome sets Referer on
// every in-site navigation). Empty by default (top-level typed URL =
// no Referer, which is also what Chrome does).
static std::string g_referer;
void setReferer(const std::string& url) { g_referer = url; }

FetchResult fetchUrl(const std::string& url, long timeoutSec) {
    ensureCurlInit();
    FetchResult res;
    CURL* curl = acquireCurl();
    if (!curl) {
        res.error = "curl init failed";
        return res;
    }

    std::string body;
    std::string headers;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
    // Empty string = send Accept-Encoding with all supported codecs and
    // transparently decompress the response (gzip, deflate, br, zstd).
    // libcurl orders them as "gzip, deflate, br, zstd" which matches
    // Chrome's actual on-the-wire order.
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    // v2.9: HTTP/2 — Chrome uses h2 by default for HTTPS. Some sites
    // (notably Google properties) treat HTTP/1.1 requests as a strong
    // bot signal because no real Chrome user is on HTTP/1.1 anymore.
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2_0);
    // v2.9: Cookie jar — read cookies the previous requests left, and
    // write any new cookies back to the same file. YouTube's first
    // response sets VISITOR_INFO1_LIVE; without the jar every fetch
    // looks like a fresh session and re-triggers the consent wall.
    curl_easy_setopt(curl, CURLOPT_COOKIEFILE, cookieJarPath());
    curl_easy_setopt(curl, CURLOPT_COOKIEJAR, cookieJarPath());
    // v2.7: pretend to be a real Chrome on Linux so the actual services
    // (YouTube, Google search, DuckDuckGo) serve their real pages instead
    // of bot-variant / "needs JS" stubs. The previous "MiniBrowser/2.6"
    // UA made YouTube serve a consent interstitial and Google serve a JS
    // shell, which forced us into the Lite-page shims. With a real browser
    // UA + Sec-Fetch-* / Sec-CH-UA headers, sites classify the request as
    // a normal Chrome navigation and ship the same HTML they'd ship to a
    // desktop user.
    curl_easy_setopt(curl, CURLOPT_USERAGENT,
                     "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
                     "(KHTML, like Gecko) Chrome/120.0.6099.109 Safari/537.36");
    // Headers: the full Chrome navigation bundle. v2.9 adds the FULL
    // Chrome client-hints bundle (Sec-CH-UA-Full-Version-List, Arch,
    // Bitness, Model, Platform-Version, Form-Factors, WoW64, DPR,
    // Viewport-Width, Width, Device-Memory, X-Client-Data, Priority)
    // — these are the headers Chrome 120 sends on every navigation
    // that v2.8 was missing. Without Sec-CH-UA-Full-Version-List Google
    // in particular downgrades to the JS-only shell. Plus consent
    // cookies for YouTube (kept from v2.6 — they are NOT a shim, they
    // just encode the EU consent choice that a real Chrome session on
    // an EU IP would also persist). PREF pins the UI language so the
    // page stays predictable.
    {
        static std::string g_langValue = [] {
            const char* e = getenv("MB_LANG");
            return (e && *e) ? std::string(e) : std::string("en-US,en;q=0.9");
        }();
        curl_slist* reqHeaders = nullptr;
        reqHeaders = curl_slist_append(reqHeaders,
            "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,"
            "image/avif,image/webp,image/apng,*/*;q=0.8,"
            "application/signed-exchange;v=b3;q=0.7");
        reqHeaders = curl_slist_append(reqHeaders,
            ("Accept-Language: " + g_langValue).c_str());
        reqHeaders = curl_slist_append(reqHeaders,
            "Upgrade-Insecure-Requests: 1");
        // Chrome client hints (basic). Google in particular checks
        // Sec-CH-UA to decide whether to ship the rich search page or
        // the legacy HTML fallback. Without these Google answers with a
        // bare "you need JS" page even to a real Chrome UA on a non-EU
        // IP.
        reqHeaders = curl_slist_append(reqHeaders,
            "Sec-CH-UA: \"Google Chrome\";v=\"120\", "
            "\"Chromium\";v=\"120\", \"Not?A_Brand\";v=\"24\"");
        reqHeaders = curl_slist_append(reqHeaders,
            "Sec-CH-UA-Mobile: ?0");
        reqHeaders = curl_slist_append(reqHeaders,
            "Sec-CH-UA-Platform: \"Linux\"");
        // v2.9: full Chrome client-hints bundle. Chrome 120 sends these
        // on every navigation; missing them is a clear bot signal.
        reqHeaders = curl_slist_append(reqHeaders,
            "Sec-CH-UA-Full-Version-List: \"Google Chrome\";v=\"120.0.6099.109\", "
            "\"Chromium\";v=\"120.0.6099.109\", "
            "\"Not?A_Brand\";v=\"24.0.0.0\"");
        reqHeaders = curl_slist_append(reqHeaders,
            "Sec-CH-UA-Full-Version: \"120.0.6099.109\"");
        reqHeaders = curl_slist_append(reqHeaders,
            "Sec-CH-UA-Arch: \"x86\"");
        reqHeaders = curl_slist_append(reqHeaders,
            "Sec-CH-UA-Bitness: \"64\"");
        reqHeaders = curl_slist_append(reqHeaders,
            "Sec-CH-UA-Model: \"\"");
        reqHeaders = curl_slist_append(reqHeaders,
            "Sec-CH-UA-Platform-Version: \"6.5.0\"");
        reqHeaders = curl_slist_append(reqHeaders,
            "Sec-CH-UA-Form-Factors: \"Desktop\"");
        reqHeaders = curl_slist_append(reqHeaders,
            "Sec-CH-UA-WoW64: ?0");
        reqHeaders = curl_slist_append(reqHeaders,
            "Sec-CH-DPR: 1");
        reqHeaders = curl_slist_append(reqHeaders,
            "Sec-CH-Viewport-Width: 1024");
        reqHeaders = curl_slist_append(reqHeaders,
            "Sec-CH-Width: 1024");
        reqHeaders = curl_slist_append(reqHeaders,
            "Device-Memory: 8");
        reqHeaders = curl_slist_append(reqHeaders,
            "X-Client-Data: CIm2yQEIpt3JAULckK8FBLjTzAEIzufA");
        reqHeaders = curl_slist_append(reqHeaders,
            "Priority: u=0, i");
        // v2.9: suppress the Expect: 100-continue header that libcurl
        // adds by default. Real Chrome never sends it on GETs; its
        // presence is a small but real bot signal.
        reqHeaders = curl_slist_append(reqHeaders, "Expect:");
        // Sec-Fetch-* marks this as a top-level document navigation typed
        // in by the user (Sec-Fetch-Site: none + Sec-Fetch-User: ?1). Sites
        // relax anti-CSRF / bot walls for this combination.
        reqHeaders = curl_slist_append(reqHeaders,
            "Sec-Fetch-Dest: document");
        reqHeaders = curl_slist_append(reqHeaders,
            "Sec-Fetch-Mode: navigate");
        // v2.9: when the caller set a Referer (in-site navigation),
        // send it. Otherwise Sec-Fetch-Site: none matches a top-level
        // typed URL (Chrome doesn't send Referer for those either).
        if (!g_referer.empty()) {
            reqHeaders = curl_slist_append(reqHeaders,
                ("Referer: " + g_referer).c_str());
            reqHeaders = curl_slist_append(reqHeaders,
                "Sec-Fetch-Site: same-origin");
        } else {
            reqHeaders = curl_slist_append(reqHeaders,
                "Sec-Fetch-Site: none");
        }
        reqHeaders = curl_slist_append(reqHeaders,
            "Sec-Fetch-User: ?1");
        // YouTube consent cookies are set on the FIRST visit only;
        // after that the cookie jar at $HOME/.cache/minibrowser/
        // cookies.txt carries them. We still set them here as a
        // fallback for the very first visit (the jar is empty then).
        if (isYouTubeHost(url))
            reqHeaders = curl_slist_append(reqHeaders,
                "Cookie: SOCS=CAI; "
                "CONSENT=YES+cb.20210328-17-p0.en+FX+419; "
                "PREF=hl=en&gl=US");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, reqHeaders);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT,
                         timeoutSec > 0 ? timeoutSec : 25L);
        // Abort stalled transfers (no progress for 20s).
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 20L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeToString);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, writeToString);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, &headers);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        // Accept any certificate the TLS stack accepts; keep verification ON.
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

        CURLcode rc = curl_easy_perform(curl);
        curl_slist_free_all(reqHeaders);
        if (rc != CURLE_OK) {
            res.error = curl_easy_strerror(rc);
            return res;
        }
        // v2.9: persist the in-memory cookie engine to the on-disk jar
        // ourselves. The libcurl way (CURLOPT_COOKIEJAR + curl_easy_cleanup)
        // does NOT work for us because we reuse the per-thread handle
        // across requests and never call curl_easy_cleanup — so the jar
        // never gets written. The "FLUSHALL" command word is also a
        // no-op on its own (it just clears the "new cookies" flag).
        // The actual fix: extract the cookies via CURLINFO_COOKIELIST
        // (each entry comes back in Netscape cookie file format
        // already) and write them to the jar file ourselves.
        {
            curl_slist* cookies = nullptr;
            CURLcode infoRc = curl_easy_getinfo(curl, CURLINFO_COOKIELIST, &cookies);
            if (infoRc == CURLE_OK && cookies) {
                std::string jarPath = cookieJarPath();
                FILE* f = std::fopen(jarPath.c_str(), "w");
                if (f) {
                    std::fprintf(f, "# Netscape HTTP Cookie File\n");
                    std::fprintf(f, "# https://curl.se/docs/http-cookies.html\n");
                    std::fprintf(f, "# v2.9 cookie jar — written by MiniBrowser\n\n");
                    for (curl_slist* p = cookies; p; p = p->next) {
                        if (p->data) std::fprintf(f, "%s\n", p->data);
                    }
                    std::fclose(f);
                }
                curl_slist_free_all(cookies);
            }
        }
    }

    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    char* eff = nullptr;
    curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &eff);
    if (eff) res.finalUrl = eff;

    res.status = code;
    res.body = body;
    res.contentType = headerValue(headers, "content-type");
    {
        // strip parameters: "text/html; charset=utf-8" -> "text/html"
        std::string ct = res.contentType;
        size_t semi = ct.find(';');
        if (semi != std::string::npos) ct = ct.substr(0, semi);
        std::transform(ct.begin(), ct.end(), ct.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        res.contentType = ct;
    }
    if (res.finalUrl.empty()) res.finalUrl = url;
    res.ok = (code >= 200 && code < 400);
    if (!res.ok && res.error.empty()) {
        res.error = "HTTP " + std::to_string(code);
    }
    return res;
}

// ---------------------------------------------------------------------------
// Session cache
// ---------------------------------------------------------------------------

namespace {
struct CacheEntry { FetchResult res; };
static std::mutex g_cacheMutex;
static std::unordered_map<std::string, FetchResult> g_cache;
}

FetchResult fetchUrlCached(const std::string& url, long timeoutSec) {
    {
        std::lock_guard<std::mutex> lock(g_cacheMutex);
        auto it = g_cache.find(url);
        if (it != g_cache.end()) return it->second;
    }
    FetchResult r = fetchUrl(url, timeoutSec);
    if (r.ok) {
        std::lock_guard<std::mutex> lock(g_cacheMutex);
        // Bound the cache: every entry holds a FULL response body, and a
        // long session crawling heavy pages could otherwise pin hundreds
        // of MB. On overflow the whole cache drops (same policy as the
        // texture/measure caches — it rebuilds lazily and cheaply).
        static constexpr size_t kMaxEntries = 128;
        if (g_cache.size() >= kMaxEntries) g_cache.clear();
        g_cache[url] = r;
    }
    return r;
}

void clearFetchCache() {
    std::lock_guard<std::mutex> lock(g_cacheMutex);
    g_cache.clear();
}

// ---------------------------------------------------------------------------
// In-memory image decode
// ---------------------------------------------------------------------------

// Cap decoded image dimensions (MB_MAXIMG overrides, 0 = unlimited).
// A 4000x3000 photo decodes to a ~48 MB surface plus a matching texture
// upload; on weak devices that is a RAM spike and a visible stall for an
// image that renders into a few hundred CSS pixels anyway. Downscaling
// here is visually equivalent at 1:1 display densities.
static int maxImageDim() {
    static int m = -2;
    if (m == -2) {
        const char* e = getenv("MB_MAXIMG");
        m = e ? atoi(e) : 1280;
    }
    return m;
}

SDL_Surface* decodeImageBytes(const std::string& bytes, const std::string& hintUrl) {
    if (bytes.empty()) return nullptr;
    SDL_RWops* rw = SDL_RWFromConstMem(bytes.data(), (int)bytes.size());
    if (!rw) return nullptr;
    SDL_Surface* surf = IMG_Load_RW(rw, 1);  // 1 = close rw even on failure
    if (!surf) {
        std::cerr << "[img] decode failed for " << hintUrl << ": "
                  << IMG_GetError() << "\n";
        return surf;
    }
    int maxDim = maxImageDim();
    if (maxDim > 0 && (surf->w > maxDim || surf->h > maxDim)) {
        double scale = std::min((double)maxDim / surf->w,
                                (double)maxDim / surf->h);
        int nw = std::max(1, (int)(surf->w * scale));
        int nh = std::max(1, (int)(surf->h * scale));
        SDL_Surface* conv = SDL_ConvertSurfaceFormat(surf,
                                                     SDL_PIXELFORMAT_RGBA32, 0);
        if (conv) {
            SDL_Surface* small = SDL_CreateRGBSurfaceWithFormat(
                0, nw, nh, 32, SDL_PIXELFORMAT_RGBA32);
            if (small) {
                SDL_SetSurfaceBlendMode(conv, SDL_BLENDMODE_NONE);
                if (SDL_BlitScaled(conv, nullptr, small, nullptr) == 0) {
                    std::cerr << "[img] downscaled " << surf->w << "x"
                              << surf->h << " -> " << nw << "x" << nh
                              << " (" << hintUrl << ")\n";
                    SDL_FreeSurface(conv);
                    SDL_FreeSurface(surf);
                    return small;
                }
                SDL_FreeSurface(small);
            }
            SDL_FreeSurface(conv);
        }
        // Conversion failed: keep the original surface.
        SDL_FreeSurface(surf);
        surf = IMG_Load_RW(SDL_RWFromConstMem(bytes.data(), (int)bytes.size()), 1);
    }
    return surf;
}

// ---------------------------------------------------------------------------
// data: URIs
// ---------------------------------------------------------------------------

static int hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int b64Val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

bool parseDataUri(const std::string& uri, std::string& outBytes,
                  std::string& outMime) {
    // data:[<mime>][;base64],<data>
    if (uri.size() < 5 || uri.compare(0, 5, "data:") != 0) return false;
    size_t comma = uri.find(',');
    if (comma == std::string::npos) return false;
    std::string meta = uri.substr(5, comma - 5);
    std::string payload = uri.substr(comma + 1);
    bool isB64 = false;
    size_t semi = meta.find(';');
    if (semi != std::string::npos) {
        std::string params = meta.substr(semi + 1);
        meta = meta.substr(0, semi);
        if (params.find("base64") != std::string::npos) isB64 = true;
    }
    outMime = meta;
    if (isB64) {
        outBytes.clear();
        outBytes.reserve(payload.size() / 4 * 3 + 3);
        int val = 0, valb = -8;
        for (char c : payload) {
            if (c == '=' || c == '\n' || c == '\r' || c == ' ') {
                if (c == '=') valb = -8;  // padding ends a quantum
                continue;
            }
            int v = b64Val(c);
            if (v < 0) continue;
            val = (val << 6) + v;
            valb += 6;
            if (valb >= 0) {
                outBytes += (char)((val >> valb) & 0xFF);
                valb -= 8;
            }
        }
    } else {
        // URL-encoded (percent) payload.
        outBytes.clear();
        outBytes.reserve(payload.size());
        for (size_t i = 0; i < payload.size(); ++i) {
            if (payload[i] == '%' && i + 2 < payload.size() &&
                hexVal(payload[i+1]) >= 0 && hexVal(payload[i+2]) >= 0) {
                outBytes += (char)(hexVal(payload[i+1]) * 16 + hexVal(payload[i+2]));
                i += 2;
            } else {
                outBytes += payload[i];
            }
        }
    }
    return !outBytes.empty();
}

SDL_Surface* decodeDataUriImage(const std::string& uri, const std::string& hintUrl) {
    std::string bytes, mime;
    if (!parseDataUri(uri, bytes, mime)) return nullptr;
    return decodeImageBytes(bytes, hintUrl.empty() ? "data:" + mime : hintUrl);
}

} // namespace browser
