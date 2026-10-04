// Internal header shared between jsengine.cpp and jsbindings.cpp.
// Defines JSEngine::Impl and the binding registration entry point.
#pragma once
#include "jsengine.h"
#include "duktape/duktape.h"

#include "../net/url.h"

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace browser {

// Hidden property keys on JS wrapper objects.
constexpr const char* kNodePtr  = "\xFF" "node";       // Node* on wrapper target
constexpr const char* kStylePtr = "\xFF" "stylenode";  // Node* on style target

struct JSTimer {
    int id = 0;
    std::string code;                 // for string timers
    int fnStash = -1;                 // for function timers (heap stash index)
    bool isFn = false;
    std::chrono::steady_clock::time_point fireAt;
    int intervalMs = 0;
    bool repeating = false;
    bool cancelled = false;
};

struct ListenerKey {
    Node* node = nullptr;
    std::string type;
    bool operator==(const ListenerKey& o) const {
        return node == o.node && type == o.type;
    }
};

struct ListenerKeyHash {
    size_t operator()(const ListenerKey& k) const {
        return std::hash<void*>()(k.node) ^ (std::hash<std::string>()(k.type) << 3);
    }
};

struct JSEngine::Impl {
    Impl(JSEngine* s);
    ~Impl();

    duk_context* ctx = nullptr;
    JSEngine* self = nullptr;

    std::shared_ptr<Node> documentRoot;
    std::string documentUrl;

    // Keeps DOM nodes alive while JS wrappers may still reference them.
    std::unordered_map<Node*, std::shared_ptr<Node>> alive;

    // Listeners: node+type -> heap-stash indices of handler functions.
    std::unordered_map<ListenerKey, std::vector<int>, ListenerKeyHash> listeners;
    // Document-level listeners: type -> stash ids.
    std::unordered_map<std::string, std::vector<int>> docListeners;

    int nextStashId = 1;
    std::vector<JSTimer> timers;
    int nextTimerId = 1;
    bool mutated = false;

    // --- implemented in jsengine.cpp ---
    // Push a fresh wrapper object for `node` onto the duktape stack.
    void pushNodeWrapper(std::shared_ptr<Node> node);
    // Extract the raw Node* from a wrapper anywhere on the stack.
    static Node* nodeFromStack(duk_context* ctx, duk_idx_t idx);
    // Store a function (at fnIdx) in the heap stash; pops it. Returns id.
    int stashFunction(duk_idx_t fnIdx);
    // Push the function stored at `id`.
    void pushStashed(int id);
    void notifyMutation() { mutated = true; }
    // Resolve a (possibly relative) URL against documentUrl.
    std::string resolveUrl(const std::string& u) const {
        if (documentUrl.empty()) return u;
        if (u.find("://") != std::string::npos) return u;
        if (!u.empty() && (u[0] == '#' || u[0] == '?')) return u;
        return joinUrl(documentUrl, u);
    }
};

// Defined in jsbindings.cpp: installs document/window/etc. into the heap.
void jsb_register(JSEngine::Impl* impl);

// Shared helpers (implemented in jsbindings.cpp / jsengine.cpp).
std::shared_ptr<Node> jsb_nodeFromStack(JSEngine::Impl* impl, duk_context* ctx,
                                        duk_idx_t idx);
void jsb_pushEventObject(JSEngine::Impl* impl, duk_context* ctx,
                         const std::string& type, std::shared_ptr<Node> target);

} // namespace browser
