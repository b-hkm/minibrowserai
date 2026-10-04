#include "resource.h"
#include "../net/fetch.h"
#include "../net/url.h"
#include "../html/parser.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>
#include <unordered_set>
#include <vector>

namespace browser {

ResourceLoader& ResourceLoader::instance() {
    static ResourceLoader inst;
    return inst;
}

bool ResourceLoader::fileExists(const std::string& p) {
    if (p.empty()) return false;
    std::ifstream f(p);
    return f.good();
}

static bool isAbsolute(const std::string& p) {
    if (p.empty()) return false;
    if (p[0] == '/' || p[0] == '\\') return true;
    if (p.size() >= 3 && std::isalpha((unsigned char)p[0]) &&
        p[1] == ':' && (p[2] == '/' || p[2] == '\\')) return true;
    return false;
}

// Strip query and fragment from a LOCAL path: "foo.png?v=2#bar" -> "foo.png".
// Remote URLs keep their query (CDN sizing params, signed URLs need it).
static std::string stripQueryFrag(const std::string& p) {
    auto q = p.find('?');
    auto h = p.find('#');
    size_t cut = std::min(
        q == std::string::npos ? p.size() : q,
        h == std::string::npos ? p.size() : h);
    return p.substr(0, cut);
}

// Normalize "." and ".." segments. Operates on forward-slash paths.
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

std::string ResourceLoader::resolve(const std::string& url) const {
    if (url.empty()) return url;
    // Protocol-relative "//host/img.png": inherit the page's scheme; for
    // local pages default to https (matches what the site would use).
    if (url.size() >= 2 && url[0] == '/' && url[1] == '/' && url[2] != '/') {
        if (isRemoteUrl(baseDir_)) return joinUrl(baseDir_, url);
        return "https:" + url;
    }
    // Remote URLs (http/https) and data: URIs are used as-is; remote refs
    // keep their query (CDN sizing params, signed URLs need it).
    if (isRemoteUrl(url)) return url;
    if (url.compare(0, 5, "data:") == 0) return url;

    // On a remote page EVERY relative reference — including root-relative
    // "/static/x.png", which used to fall through to the filesystem branch
    // below and never load — resolves against the page URL. joinUrl()
    // handles absolute, root-relative, "../" and query preservation.
    if (isRemoteUrl(baseDir_)) return joinUrl(baseDir_, url);

    std::string u = stripQueryFrag(url);
    if (u.empty()) return u;
    if (isAbsolute(u)) return u;

    std::string joined;
    if (!baseDir_.empty()) {
        char last = baseDir_.back();
        if (last != '/' && last != '\\') joined = baseDir_ + "/" + u;
        else                                joined = baseDir_ + u;
    } else {
        joined = u;
    }
    return normalizePath(joined);
}

// ---------------------------------------------------------------------------
// Persistent background pool
//
// The old preloadImages() spawned a fresh pool per page and JOINED it, so
// the first paint waited for every image on the page — on a slow network
// that meant a blank window for tens of seconds, which is exactly the
// "it is slow" complaint. Now:
//   - workers live for the whole session (no per-page thread churn),
//   - jobs are queued (deduped) and consumed in order,
//   - the caller NEVER blocks: loadImage() on a remote miss enqueues and
//     returns nullptr; the browser repaints when jobs land.
// ---------------------------------------------------------------------------

ResourceLoader::~ResourceLoader() { shutdown(); }

void ResourceLoader::ensureWorkersLocked_(int n) {
    if (n <= poolTarget_) return;
    poolTarget_ = n;
    for (int i = 0; i < n; ++i)
        workers_.emplace_back([this] { workerLoop_(); });
}

void ResourceLoader::workerLoop_() {
    for (;;) {
        std::string url;
        {
            std::unique_lock<std::mutex> lock(cacheMutex_);
            workCv_.wait(lock, [&] {
                return stopping_.load() || !queue_.empty();
            });
            if (stopping_.load() && queue_.empty()) return;
            url = std::move(queue_.front());
            queue_.pop_front();
        }

        CachedResource r;
        r.resolvedPath = url;
        // Shorter timeout than the default: a dead host must not stall
        // its slot for 25s.
        FetchResult fr = fetchUrlCached(url, 15);
        if (fr.ok) r.surface = decodeImageBytes(fr.body, url);
        if (r.surface) {
            r.w = r.surface->w;
            r.h = r.surface->h;
        } else {
            std::cerr << "[img] bg load failed: " << url << " ("
                      << (fr.error.empty() ? "decode error" : fr.error)
                      << ")\n";
        }

        {
            std::lock_guard<std::mutex> lock(cacheMutex_);
            inFlight_.erase(url);
            cache_[url] = r;   // stores failures too: negative cache,
                               // prevents endless re-enqueue loops
        }
        pendingJobs_.fetch_add(-1);
        arrivedFlag_.store(true);   // browser polls this from tick()
    }
}

void ResourceLoader::startPreload(const std::vector<std::string>& urls,
                                  int maxThreads) {
    // Resolve, dedupe, and keep only remote jobs — local files and data:
    // URIs decode fast enough to stay lazy.
    std::vector<std::string> jobs;
    {
        std::unordered_set<std::string> seen;
        for (const auto& u : urls) {
            if (u.empty()) continue;
            std::string r = resolve(u);
            if (r.empty() || !isRemoteUrl(r)) continue;
            if (seen.insert(r).second) jobs.push_back(r);
        }
    }
    if (jobs.empty()) return;

    if (maxThreads <= 0) {
        maxThreads = 3;   // weak devices: 3 concurrent downloads is the
                          // sweet spot; more just adds CPU contention
    }
    const char* envN = getenv("MB_IMGT");
    if (envN && *envN) maxThreads = std::max(1, atoi(envN));

    {
        std::lock_guard<std::mutex> lock(cacheMutex_);
        ensureWorkersLocked_(maxThreads);
        for (const auto& url : jobs) {
            if (cache_.count(url) || inFlight_.count(url)) continue;
            inFlight_.insert(url);
            queue_.push_back(url);
            pendingJobs_.fetch_add(1);
        }
    }
    workCv_.notify_all();
}

bool ResourceLoader::consumeImagesArrived() {
    bool expected = true;
    return arrivedFlag_.compare_exchange_strong(expected, false);
}

void ResourceLoader::waitForPendingImages(int timeoutMs) {
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeoutMs);
    while (pendingJobs_.load() > 0) {
        if (std::chrono::steady_clock::now() > deadline) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

std::string pickImageSrc(const std::shared_ptr<Node>& node) {
    auto attr = [&](const std::string& k) -> std::string {
        auto it = node->attrs.find(k);
        return it == node->attrs.end() ? "" : it->second;
    };
    std::string src = attr("src");
    if (!src.empty()) return src;
    std::string dataSrc = attr("data-src");
    if (!dataSrc.empty()) return dataSrc;
    // srcset: "url 400w, url2 800w, ..." — take the LAST (usually largest);
    // single-entry srcset without descriptor also works.
    std::string ss = attr("srcset");
    if (ss.empty()) {
        // <picture><source srcset="...">…<img></picture>
        auto p = node->parent.lock();
        if (p && p->tag == "picture") {
            for (auto& c : p->children) {
                if (c->tag == "source") {
                    auto it = c->attrs.find("srcset");
                    if (it != c->attrs.end() && !it->second.empty()) { ss = it->second; break; }
                }
            }
        }
    }
    if (!ss.empty()) {
        std::string last;
        size_t start = 0;
        while (start <= ss.size()) {
            size_t comma = ss.find(',', start);
            std::string cand = ss.substr(start,
                    comma == std::string::npos ? std::string::npos : comma - start);
            cand = cand.substr(0, cand.find_first_of(" \t"));  // strip descriptor
            if (!cand.empty()) last = cand;
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
        return last;
    }
    return "";
}

CachedResource* ResourceLoader::loadImage(const std::string& url) {
    if (url.empty()) return nullptr;

    // data: URIs decode straight from memory.
    if (url.compare(0, 5, "data:") == 0) {
        auto it = cache_.find(url);
        if (it != cache_.end()) {
            return it->second.surface ? &it->second : nullptr;
        }
        CachedResource r;
        r.resolvedPath = "(data uri)";
        r.surface = decodeDataUriImage(url, "img[data uri]");
        if (r.surface) {
            r.w = r.surface->w;
            r.h = r.surface->h;
        } else {
            std::cerr << "[img] data uri decode failed\n";
        }
        cache_[url] = r;
        return r.surface ? &cache_[url] : nullptr;
    }

    // Resolve FIRST, then check whether the result is remote. This covers:
    //   - absolute http(s) URLs
    //   - protocol-relative //host/img.png
    //   - relative paths on a remote page (baseDir_ is the page URL) —
    //     the old code resolved these to a remote URL and then treated
    //     the URL as a filesystem path, so NO relative image on any
    //     remote page ever loaded.
    std::string resolved = resolve(url);
    if (resolved.empty()) return nullptr;

    if (isRemoteUrl(resolved)) {
        auto it = cache_.find(resolved);
        if (it != cache_.end()) {
            return it->second.surface ? &it->second : nullptr;
        }
        // Remote miss: NEVER download on the calling thread — that used to
        // stall layout/paint for up to 15s per image. Enqueue a background
        // job and show the placeholder until it lands (the browser polls
        // consumeImagesArrived() and schedules a relayout).
        {
            std::lock_guard<std::mutex> lock(cacheMutex_);
            if (!inFlight_.count(resolved)) {
                ensureWorkersLocked_(3);
                inFlight_.insert(resolved);
                queue_.push_back(resolved);
                pendingJobs_.fetch_add(1);
                workCv_.notify_one();
            }
        }
        return nullptr;
    }

    auto it = cache_.find(resolved);
    if (it != cache_.end()) {
        return it->second.surface ? &it->second : nullptr;
    }

    CachedResource r;
    r.resolvedPath = resolved;

    if (!fileExists(resolved)) {
        std::cerr << "[img] not found: '" << url << "' (resolved: '"
                  << resolved << "')\n";
        cache_[resolved] = r;
        return nullptr;
    }

    r.surface = IMG_Load(resolved.c_str());
    if (!r.surface) {
        std::cerr << "[img] decode failed: '" << resolved
                  << "': " << IMG_GetError() << "\n";
        cache_[resolved] = r;
        return nullptr;
    }

    r.w = r.surface->w;
    r.h = r.surface->h;
    std::cerr << "[img] loaded: '" << url << "' -> " << r.w << "x" << r.h
              << " from " << resolved << "\n";
    cache_[resolved] = r;
    return &cache_[resolved];
}

void ResourceLoader::shutdown() {
    // Stop the pool first: workers may be mid-decode, and their next loop
    // iteration must see stopping_ before they touch anything again.
    {
        std::lock_guard<std::mutex> lock(cacheMutex_);
        stopping_.store(true);
        queue_.clear();
    }
    workCv_.notify_all();
    for (auto& th : workers_) {
        if (th.joinable()) th.detach();
    }
    workers_.clear();
    for (auto& [k, v] : cache_) {
        (void)k;
        if (v.surface) SDL_FreeSurface(v.surface);
    }
    cache_.clear();
}

} // namespace browser
