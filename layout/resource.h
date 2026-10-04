#pragma once
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace browser {

struct Node;

// One loaded resource keyed by its *resolved* absolute path.
struct CachedResource {
    SDL_Surface* surface = nullptr;
    int w = 0, h = 0;
    std::string resolvedPath;  // what we actually opened
};

// Back-compat: CachedImage is the old name, same layout.
using CachedImage = CachedResource;

// Owns a base directory and resolves relative URLs/paths against it.
//
// Threading model (weak-device friendly — the UI thread never waits):
//   - Local files and data: URIs decode synchronously (fast, in-memory).
//   - Remote images NEVER block the caller. loadImage() on a miss kicks
//     off a background fetch+decode job and returns nullptr; the renderer
//     shows the grey alt-text placeholder until the job lands, tick()
//     notices arrivals and schedules a debounced relayout.
class ResourceLoader {
public:
    explicit ResourceLoader(std::string baseDir = "") : baseDir_(std::move(baseDir)) {}
    ~ResourceLoader();

    std::string resolve(const std::string& url) const;

    static bool fileExists(const std::string& p);

    // Returns the entry on hit, nullptr on miss. Pointer valid until shutdown().
    // A remote miss starts a background job (never blocks, never downloads
    // on the calling thread).
    CachedResource* loadImage(const std::string& url);

    // Enqueue a batch of image URLs for background fetch+decode and return
    // immediately. Replaces the old joining preloadImages(): first paint no
    // longer waits for the whole gallery. Deduplicated against the cache and
    // anything already queued. `maxThreads` caps the persistent pool size.
    void startPreload(const std::vector<std::string>& urls, int maxThreads = 3);

    // Number of queued or in-flight remote image jobs.
    int pendingPreloads() const { return pendingJobs_.load(); }

    // True (and clears) when at least one image landed since the last call.
    // The browser polls this from tick() to repaint / relayout.
    bool consumeImagesArrived();

    // Block until no remote jobs remain (bounded by `timeoutMs`). Used by
    // --screenshot mode only, which must render a complete page in one shot.
    void waitForPendingImages(int timeoutMs);

    void setBaseDir(std::string d) { baseDir_ = std::move(d); }
    const std::string& baseDir() const { return baseDir_; }

    void shutdown();

    static ResourceLoader& instance();

private:
    std::string baseDir_;
    std::unordered_map<std::string, CachedResource> cache_;
    std::mutex cacheMutex_;  // guards cache_ / queue_ / inFlight_

    // Persistent background pool (created lazily on the first remote job).
    std::vector<std::thread> workers_;
    std::condition_variable workCv_;
    void ensureWorkersLocked_(int n);
    void workerLoop_();
    std::deque<std::string> queue_;
    std::unordered_set<std::string> inFlight_;  // queued or running
    std::atomic<int>  pendingJobs_{0};          // queued + running, atomic for cheap polling
    std::atomic<bool> arrivedFlag_{false};      // >=1 image landed since last consume
    std::atomic<bool> stopping_{false};
    int poolTarget_ = 0;                        // requested worker count
};

// Pick the best source URL for an <img>: src, then data-src (lazy
// loaders), then the last srcset candidate, then a <source srcset> from
// a parent <picture>. Returns "" when none. Shared by layout and the
// preloader so both agree on which URL an <img> will use.
std::string pickImageSrc(const std::shared_ptr<Node>& node);

// Back-compat free functions. The shim uses CachedImage, which is now
// an alias for CachedResource, so this is type-safe with no reinterpret_cast.
inline CachedImage* getImage(const std::string& url) {
    return ResourceLoader::instance().loadImage(url);
}
inline void shutdownImages() { ResourceLoader::instance().shutdown(); }

} // namespace browser

