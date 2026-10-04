// JSEngine core: duktape heap management, node wrapper construction,
// event dispatch, timers, and the public API. The DOM bindings live in
// jsbindings.cpp.
#include "js_internal.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <iostream>
#include <thread>

namespace browser {

// ---------------------------------------------------------------------------
// Impl method implementations
// ---------------------------------------------------------------------------

// Read the hidden Node* from a wrapper (works on the Proxy or its target:
// the Proxy get trap forwards unknown keys to the target).
Node* JSEngine::Impl::nodeFromStack(duk_context* ctx, duk_idx_t idx) {
    duk_get_prop_string(ctx, idx, kNodePtr);
    if (!duk_is_pointer(ctx, -1)) { duk_pop(ctx); return nullptr; }
    void* p = duk_get_pointer(ctx, -1);
    duk_pop(ctx);
    return static_cast<Node*>(p);
}

void JSEngine::Impl::pushNodeWrapper(std::shared_ptr<Node> node) {
    if (!node) { duk_push_null(ctx); return; }
    // Keep the node alive as long as a wrapper may reference it.
    alive[node.get()] = node;

    duk_push_object(ctx);                       // [target]
    // The methods prototype is attached inside __makeNode (JS) via
    // Object.setPrototypeOf — far less error-prone than the C stack
    // shuffle this used to do.
    // Hidden Node* on the TARGET (the trap reads it from here).
    duk_push_pointer(ctx, (void*)node.get());
    duk_put_prop_string(ctx, -2, kNodePtr);
    // Wrap in a Proxy with get/set traps.
    duk_get_global_string(ctx, "__makeNode");
    if (!duk_is_function(ctx, -1)) {
        // Bootstrap failed (no Proxy support) — return the raw target.
        duk_pop(ctx);
        return;
    }
    duk_dup(ctx, -2);
    duk_call(ctx, 1);
    duk_remove(ctx, -2);                        // drop the target
}

int JSEngine::Impl::stashFunction(duk_idx_t fnIdx) {
    int id = nextStashId++;
    duk_push_heap_stash(ctx);                   // [.., stash]
    duk_dup(ctx, fnIdx);                        // [.., stash, fn]
    duk_put_prop_index(ctx, -2, (duk_uarridx_t)id);  // stash[id] = fn
    duk_pop(ctx);                               // [..]
    return id;
}

void JSEngine::Impl::pushStashed(int id) {
    duk_push_heap_stash(ctx);
    duk_get_prop_index(ctx, -1, (duk_uarridx_t)id);
    duk_remove(ctx, -2);
}

// ---------------------------------------------------------------------------
// Shared helpers used by jsbindings.cpp (declared in js_internal.h)
// ---------------------------------------------------------------------------

std::shared_ptr<Node> jsb_nodeFromStack(JSEngine::Impl* impl, duk_context* ctx,
                                        duk_idx_t idx) {
    (void)impl;
    Node* raw = JSEngine::Impl::nodeFromStack(ctx, idx);
    if (!raw) return nullptr;
    auto it = impl->alive.find(raw);
    return (it != impl->alive.end()) ? it->second : nullptr;
}

void jsb_pushEventObject(JSEngine::Impl* impl, duk_context* ctx,
                         const std::string& type,
                         std::shared_ptr<Node> target) {
    duk_push_object(ctx);
    duk_push_string(ctx, type.c_str());
    duk_put_prop_string(ctx, -2, "type");
    if (target) {
        impl->pushNodeWrapper(target);
        duk_put_prop_string(ctx, -2, "target");
        auto it = target->attrs.find("id");
        if (it != target->attrs.end()) {
            duk_push_string(ctx, it->second.c_str());
            duk_put_prop_string(ctx, -2, "targetId");
        }
    }
}

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

JSEngine::JSEngine() : impl(new Impl(this)) {}

JSEngine::~JSEngine() {
    delete impl;
}

JSEngine::Impl::Impl(JSEngine* s) : self(s) {
    ctx = duk_create_heap_default();
    if (!ctx) {
        std::cerr << "[fatal] could not create duktape heap\n";
        return;
    }
    // The bindings need `self` set before registration; jsb_register
    // stores the Impl pointer in the heap stash for the C bindings.
    jsb_register(this);
}

JSEngine::Impl::~Impl() {
    if (ctx) duk_destroy_heap(ctx);
}

// ---------------------------------------------------------------------------
// Execution
// ---------------------------------------------------------------------------

bool JSEngine::execute(const std::string& code) {
    if (!impl->ctx || code.empty()) return false;
    impl->mutated = false;
    duk_context* ctx = impl->ctx;
    duk_int_t rc = duk_peval_string(ctx, code.c_str());
    if (rc != DUK_EXEC_SUCCESS) {
        duk_get_prop_string(ctx, -1, "stack");
        const char* stack = duk_safe_to_string(ctx, -1);
        std::cerr << "[JS Error] " << (stack ? stack : "?") << "\n";
    }
    duk_pop(ctx);
    if (impl->mutated) {
        impl->mutated = false;
        if (onDomMutated) onDomMutated();
    }
    return rc == DUK_EXEC_SUCCESS;
}

bool JSEngine::executeEvent(const std::string& code, const std::string& type,
                            std::shared_ptr<Node> target) {
    if (!impl->ctx || code.empty()) return false;
    impl->mutated = false;
    duk_context* ctx = impl->ctx;

    // Build a real `event` object and expose it as a global for the
    // handler code, then remove the global afterwards.
    jsb_pushEventObject(impl, ctx, type, target);
    duk_put_global_string(ctx, "event");

    duk_int_t rc = duk_peval_string(ctx, code.c_str());
    if (rc != DUK_EXEC_SUCCESS) {
        duk_get_prop_string(ctx, -1, "stack");
        const char* stack = duk_safe_to_string(ctx, -1);
        std::cerr << "[JS Error] " << (stack ? stack : "?") << "\n";
    }
    duk_pop(ctx);

    duk_push_global_object(ctx);
    duk_del_prop_string(ctx, -1, "event");
    duk_pop(ctx);

    bool ran = (rc == DUK_EXEC_SUCCESS);
    if (impl->mutated) {
        impl->mutated = false;
        if (onDomMutated) onDomMutated();
    }
    return ran;
}

bool JSEngine::dispatchEvent(const std::string& type,
                             std::shared_ptr<Node> target) {
    if (!impl->ctx || !target) return false;
    impl->mutated = false;
    duk_context* ctx = impl->ctx;

    // The event object stays on the stack while handlers run.
    jsb_pushEventObject(impl, ctx, type, target);
    duk_idx_t eventIdx = duk_normalize_index(ctx, -1);
    // Also expose as `event` global (some inline handlers expect it).
    duk_dup(ctx, eventIdx);
    duk_put_global_string(ctx, "event");

    bool ran = false;

    // 1. Listeners on the target and up the ancestor chain (bubbling).
    std::shared_ptr<Node> n = target;
    while (n) {
        auto it = impl->listeners.find({n.get(), type});
        if (it != impl->listeners.end()) {
            for (int id : it->second) {
                impl->pushStashed(id);
                if (!duk_is_function(ctx, -1)) { duk_pop(ctx); continue; }
                duk_dup(ctx, eventIdx);
                duk_int_t rc = duk_pcall(ctx, 1);
                if (rc != DUK_EXEC_SUCCESS) {
                    duk_get_prop_string(ctx, -1, "stack");
                    const char* stack = duk_safe_to_string(ctx, -1);
                    std::cerr << "[JS Error] " << (stack ? stack : "?") << "\n";
                    duk_pop_2(ctx);
                } else {
                    duk_pop(ctx);
                    ran = true;
                }
            }
        }
        n = n->parent.lock();
    }

    // 2. Document-level listeners last.
    auto dit = impl->docListeners.find(type);
    if (dit != impl->docListeners.end()) {
        for (int id : dit->second) {
            impl->pushStashed(id);
            if (!duk_is_function(ctx, -1)) { duk_pop(ctx); continue; }
            duk_dup(ctx, eventIdx);
            duk_int_t rc = duk_pcall(ctx, 1);
            if (rc != DUK_EXEC_SUCCESS) {
                duk_get_prop_string(ctx, -1, "stack");
                const char* stack = duk_safe_to_string(ctx, -1);
                std::cerr << "[JS Error] " << (stack ? stack : "?") << "\n";
                duk_pop_2(ctx);
            } else {
                duk_pop(ctx);
                ran = true;
            }
        }
    }

    duk_pop(ctx);   // event object

    // Remove the temporary `event` global again.
    duk_push_global_object(ctx);
    duk_del_prop_string(ctx, -1, "event");
    duk_pop(ctx);

    if (impl->mutated) {
        impl->mutated = false;
        if (onDomMutated) onDomMutated();
    }
    return ran;
}

bool JSEngine::callFunction(const std::string& name) {
    if (!impl->ctx) return false;
    duk_context* ctx = impl->ctx;
    impl->mutated = false;
    duk_get_global_string(ctx, name.c_str());
    if (!duk_is_function(ctx, -1)) {
        duk_pop(ctx);
        return false;
    }
    duk_int_t rc = duk_pcall(ctx, 0);
    if (rc != DUK_EXEC_SUCCESS) {
        duk_get_prop_string(ctx, -1, "stack");
        const char* stack = duk_safe_to_string(ctx, -1);
        std::cerr << "[JS Error] " << (stack ? stack : "?") << "\n";
    }
    duk_pop(ctx);
    if (impl->mutated) {
        impl->mutated = false;
        if (onDomMutated) onDomMutated();
    }
    return true;
}

// ---------------------------------------------------------------------------
// Timers
// ---------------------------------------------------------------------------

int JSEngine::nextTimerDelayMs() const {
    if (!impl || !impl->ctx) return -1;
    if (impl->timers.empty()) return -1;
    auto now = std::chrono::steady_clock::now();
    long long best = -1;
    for (const auto& t : impl->timers) {
        if (t.cancelled) continue;
        long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                           t.fireAt - now).count();
        if (ms < 0) ms = 0;
        if (best < 0 || ms < best) best = ms;
        if (best == 0) break;
    }
    return best > (long long)std::numeric_limits<int>::max()
               ? std::numeric_limits<int>::max() : (int)best;
}

bool JSEngine::pumpTimers() {
    if (!impl->ctx) return false;
    bool any = false;
    auto now = std::chrono::steady_clock::now();

    // Iterate over a snapshot: handlers may register new timers.
    std::vector<JSTimer> due;
    for (auto& t : impl->timers) {
        if (t.cancelled) continue;
        if (now < t.fireAt) continue;
        due.push_back(t);
        if (t.repeating)
            t.fireAt = now + std::chrono::milliseconds(std::max(1, t.intervalMs));
        else
            t.cancelled = true;
    }

    for (auto& t : due) {
        impl->mutated = false;
        if (t.isFn) {
            impl->pushStashed(t.fnStash);
            if (duk_is_function(impl->ctx, -1)) {
                duk_int_t rc = duk_pcall(impl->ctx, 0);
                if (rc != DUK_EXEC_SUCCESS) {
                    duk_get_prop_string(impl->ctx, -1, "stack");
                    const char* stack = duk_safe_to_string(impl->ctx, -1);
                    std::cerr << "[JS Error] " << (stack ? stack : "?") << "\n";
                }
                duk_pop(impl->ctx);
                any = true;
            } else {
                duk_pop(impl->ctx);
            }
        } else if (!t.code.empty()) {
            duk_int_t rc = duk_peval_string(impl->ctx, t.code.c_str());
            if (rc != DUK_EXEC_SUCCESS) {
                duk_get_prop_string(impl->ctx, -1, "stack");
                const char* stack = duk_safe_to_string(impl->ctx, -1);
                std::cerr << "[JS Error] " << (stack ? stack : "?") << "\n";
            }
            duk_pop(impl->ctx);
            any = true;
        }
        if (impl->mutated && onDomMutated) onDomMutated();
    }

    // Periodically drop cancelled timers.
    if (impl->timers.size() > 64) {
        impl->timers.erase(
            std::remove_if(impl->timers.begin(), impl->timers.end(),
                           [](const JSTimer& t) { return t.cancelled; }),
            impl->timers.end());
    }

    if (any && onInvalidate) onInvalidate();
    return any;
}

// ---------------------------------------------------------------------------
// Document
// ---------------------------------------------------------------------------

void JSEngine::setDocument(std::shared_ptr<Node> doc) {
    impl->documentRoot = doc;
    impl->alive.clear();
    impl->listeners.clear();
    impl->docListeners.clear();
    impl->timers.clear();
    if (doc) impl->alive[doc.get()] = doc;
}

std::shared_ptr<Node> JSEngine::getDocument() const {
    return impl->documentRoot;
}

void JSEngine::setDocumentUrl(const std::string& url) {
    impl->documentUrl = url;
}

std::string JSEngine::evaluateToString(const std::string& expr) {
    if (!impl->ctx) return "";
    duk_context* ctx = impl->ctx;
    duk_int_t rc = duk_peval_string(ctx, expr.c_str());
    std::string out;
    if (rc == DUK_EXEC_SUCCESS) {
        const char* s = duk_safe_to_string(ctx, -1);
        if (s) out = s;
    } else {
        duk_get_prop_string(ctx, -1, "stack");
        const char* stack = duk_safe_to_string(ctx, -1);
        out = std::string("<error> ") + (stack ? stack : "?");
    }
    duk_pop(ctx);
    return out;
}

} // namespace browser
