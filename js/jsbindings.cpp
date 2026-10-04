// Duktape <-> DOM bindings for the mini browser's JS engine.
// Everything JavaScript can touch about the document lives here.
//
// Node wrappers are ES6 Proxies: the get/set traps compute DOM
// properties on demand (textContent, style, children, ...) and forward
// everything else to a plain target object that carries the methods and
// a hidden Node pointer.
#include "js_internal.h"

#include "../media/mediaplayer.h"
#include "../net/fetch.h"
#include "../net/url.h"

#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <iostream>
#include <sstream>

namespace browser {

// Global API additions (defined later, before jsb_register).
static duk_ret_t js_btoa(duk_context* ctx);
static duk_ret_t js_atob(duk_context* ctx);
static duk_ret_t js_encodeURIComponent(duk_context* ctx);
static duk_ret_t js_decodeURIComponent(duk_context* ctx);
static duk_ret_t js_requestAnimationFrame(duk_context* ctx);
static duk_ret_t js_cancelAnimationFrame(duk_context* ctx);

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

static std::string jbTrim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// Truthiness per JS semantics (this vendored duktape lacks duk_truthy).
static bool dukIsTruthy(duk_context* ctx, duk_idx_t idx) {
    duk_dup(ctx, idx);
    bool v = duk_to_boolean(ctx, -1) != 0;
    duk_pop(ctx);
    return v;
}

static std::string jbLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

static std::string jbUpper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::toupper(c); });
    return s;
}

static std::string concatText(const std::shared_ptr<Node>& n) {
    if (!n) return "";
    if (n->tag == "text") return n->text;
    std::string out;
    for (auto& c : n->children) out += concatText(c);
    return out;
}

static std::string escapeHtml(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '&':  out += "&amp;"; break;
            case '<':  out += "&lt;";  break;
            case '>':  out += "&gt;";  break;
            default:   out += c;
        }
    }
    return out;
}

static std::string serializeChildren(const std::shared_ptr<Node>& n) {
    std::string out;
    if (!n) return out;
    static const char* kVoid[] = {"br","img","hr","input","meta","link","area",
                                  "base","col","embed","param","source","track",
                                  "wbr", nullptr};
    auto isVoid = [&](const std::string& t) {
        for (int i = 0; kVoid[i]; ++i) if (t == kVoid[i]) return true;
        return false;
    };
    for (auto& c : n->children) {
        if (c->tag == "text") {
            out += escapeHtml(c->text);
        } else {
            out += "<" + c->tag;
            for (auto& [k, v] : c->attrs) out += " " + k + "=\"" + escapeHtml(v) + "\"";
            out += ">";
            out += serializeChildren(c);
            if (!isVoid(c->tag)) out += "</" + c->tag + ">";
        }
    }
    return out;
}

static bool dukIsStr(duk_context* ctx, duk_idx_t idx) {
    return duk_is_string(ctx, idx) && !duk_is_symbol(ctx, idx);
}

static std::string dukToStdString(duk_context* ctx, duk_idx_t idx) {
    const char* s = duk_to_string(ctx, idx);
    return s ? std::string(s) : std::string();
}

namespace {

// Forward decls for the classList methods (defined below in this
// anonymous namespace; the node property trap references them earlier).
static duk_ret_t cl_add(duk_context* ctx);
static duk_ret_t cl_remove(duk_context* ctx);
static duk_ret_t cl_toggle(duk_context* ctx);
static duk_ret_t cl_contains(duk_context* ctx);
static duk_ret_t cl_item(duk_context* ctx);
static duk_ret_t cl_value(duk_context* ctx);
static duk_ret_t cl_length(duk_context* ctx);

JSEngine::Impl* getImpl(duk_context* ctx) {
    duk_push_heap_stash(ctx);
    duk_get_prop_index(ctx, -1, 0);
    void* p = duk_require_pointer(ctx, -1);
    duk_pop_2(ctx);
    return static_cast<JSEngine::Impl*>(p);
}

Node* nodeFromDirect(duk_context* ctx, duk_idx_t idx) {
    duk_get_prop_string(ctx, idx, kNodePtr);
    if (!duk_is_pointer(ctx, -1)) { duk_pop(ctx); return nullptr; }
    void* p = duk_get_pointer(ctx, -1);
    duk_pop(ctx);
    return static_cast<Node*>(p);
}


// `this` for method bindings: duk_push_this() + read hidden Node*.
// (Method C functions do NOT receive `this` at index 0 — only the
// proxy get/set traps get the target as arg 0.)
Node* thisNodeRaw(duk_context* ctx) {
    duk_push_this(ctx);
    Node* n = nodeFromDirect(ctx, -1);
    duk_pop(ctx);
    return n;
}

std::shared_ptr<Node> sharedNode(JSEngine::Impl* impl, Node* raw) {
    if (!raw) return nullptr;
    auto it = impl->alive.find(raw);
    return (it != impl->alive.end()) ? it->second : nullptr;
}

// camelCase -> kebab-case
static std::string kebab(const std::string& jsName) {
    std::string out;
    for (char c : jsName) {
        if (std::isupper((unsigned char)c)) {
            out += '-';
            out += (char)std::tolower((unsigned char)c);
        } else {
            out += c;
        }
    }
    return out;
}

static void setNodeStyle(std::shared_ptr<Node> node, const std::string& prop,
                         const std::string& value) {
    if (!node) return;
    std::string want = kebab(prop);
    std::string cur = node->attrs.count("style") ? node->attrs["style"] : "";
    std::vector<std::pair<std::string, std::string>> decls;
    std::string::size_type p = 0;
    while (p < cur.size()) {
        auto semi = cur.find(';', p);
        std::string part = jbTrim(cur.substr(p, semi - p));
        if (!part.empty()) {
            auto colon = part.find(':');
            if (colon != std::string::npos)
                decls.push_back({jbTrim(part.substr(0, colon)),
                                 jbTrim(part.substr(colon + 1))});
        }
        if (semi == std::string::npos) break;
        p = semi + 1;
    }
    bool replaced = false;
    for (auto& d : decls)
        if (d.first == want) { d.second = value; replaced = true; }
    if (!replaced) decls.push_back({want, value});
    std::string out;
    for (size_t i = 0; i < decls.size(); ++i) {
        if (i) out += "; ";
        out += decls[i].first + ": " + decls[i].second;
    }
    node->attrs["style"] = out;
}

static std::string getNodeStyle(std::shared_ptr<Node> node, const std::string& prop) {
    if (!node || !node->attrs.count("style")) return "";
    std::string want = kebab(prop);
    std::string cur = node->attrs["style"];
    std::string::size_type p = 0;
    while (p < cur.size()) {
        auto semi = cur.find(';', p);
        std::string part = jbTrim(cur.substr(p, semi - p));
        auto colon = part.find(':');
        if (colon != std::string::npos && jbTrim(part.substr(0, colon)) == want)
            return jbTrim(part.substr(colon + 1));
        if (semi == std::string::npos) break;
        p = semi + 1;
    }
    return "";
}

// Build a JS array of wrappers for a node list.
static void pushWrapperArray(JSEngine::Impl* impl,
                             const std::vector<std::shared_ptr<Node>>& nodes) {
    duk_idx_t arr = duk_push_array(impl->ctx);
    duk_uarridx_t i = 0;
    for (auto& n : nodes) {
        impl->pushNodeWrapper(n);
        duk_put_prop_index(impl->ctx, arr, i++);
    }
}

// Simple querySelector engine: "tag", ".class", "#id", "tag.class", "a.b#c".
static bool simpleSelectorMatches(const std::string& sel,
                                  const std::shared_ptr<Node>& n) {
    if (!n || n->tag == "text" || n->tag == "root") return false;
    std::string tag, cls, id;
    size_t i = 0;
    while (i < sel.size()) {
        if (sel[i] == '.') {
            size_t j = ++i;
            while (i < sel.size() && sel[i] != '.' && sel[i] != '#') ++i;
            cls = sel.substr(j, i - j);
        } else if (sel[i] == '#') {
            size_t j = ++i;
            while (i < sel.size() && sel[i] != '.' && sel[i] != '#') ++i;
            id = sel.substr(j, i - j);
        } else {
            size_t j = i;
            while (i < sel.size() && sel[i] != '.' && sel[i] != '#') ++i;
            tag = jbLower(sel.substr(j, i - j));
        }
    }
    if (!tag.empty() && tag != "*" && tag != n->tag) return false;
    if (!id.empty()) {
        auto it = n->attrs.find("id");
        if (it == n->attrs.end() || it->second != id) return false;
    }
    if (!cls.empty()) {
        auto it = n->attrs.find("class");
        if (it == n->attrs.end()) return false;
        std::istringstream ss(it->second);
        std::string c;
        bool found = false;
        while (ss >> c) if (jbLower(c) == cls) { found = true; break; }
        if (!found) return false;
    }
    return true;
}

static void walkSelector(const std::string& sel,
                         const std::shared_ptr<Node>& n,
                         std::vector<std::shared_ptr<Node>>& out) {
    for (auto& c : n->children) {
        if (simpleSelectorMatches(sel, c)) out.push_back(c);
        walkSelector(sel, c, out);
    }
}

// ---------------------------------------------------------------------------
// HTMLMediaElement-lite (round 9): <video>/<audio> backed by the internal
// player registry. Property reads/writes and play()/pause() all funnel
// through mediaSourceUrlFor + a registry lookup, so page JS, the layout
// and the renderer can never disagree about WHICH player they mean.
// ---------------------------------------------------------------------------

// Same resolution rule as layoutMedia(): src attribute, then the first
// <source src> child, then resolved against the current document.
static std::string mediaSourceUrlFor(JSEngine::Impl* impl,
                                     const std::shared_ptr<Node>& node) {
    if (!node) return "";
    std::string s;
    auto it = node->attrs.find("src");
    if (it != node->attrs.end() && !it->second.empty()) s = it->second;
    if (s.empty()) {
        for (auto& c : node->children) {
            if (c->tag == "source") {
                auto sIt = c->attrs.find("src");
                if (sIt != c->attrs.end() && !sIt->second.empty()) {
                    s = sIt->second;
                    break;
                }
            }
        }
    }
    if (s.empty()) return "";
    if (impl->self && impl->self->resolveMedia) return impl->self->resolveMedia(s);
    return impl->resolveUrl(s);
}

static std::shared_ptr<media::MediaPlayer> mediaPlayerFromStack(
        JSEngine::Impl* impl, duk_context* ctx) {
    auto node = sharedNode(impl, thisNodeRaw(ctx));   // `this` = node proxy
    if (!node || (node->tag != "video" && node->tag != "audio")) return nullptr;
    std::string url = mediaSourceUrlFor(impl, node);
    if (url.empty()) return nullptr;
    return media::findPlayer(url);
}

static duk_ret_t nm_media_play(duk_context* ctx) {
    if (auto p = mediaPlayerFromStack(getImpl(ctx), ctx)) p->play();
    return 0;
}

static duk_ret_t nm_media_pause(duk_context* ctx) {
    if (auto p = mediaPlayerFromStack(getImpl(ctx), ctx)) p->pause();
    return 0;
}

// ---------------------------------------------------------------------------
// Node property traps
// ---------------------------------------------------------------------------

duk_ret_t nodeHandlerGet(duk_context* ctx) {
    JSEngine::Impl* impl = getImpl(ctx);
    Node* raw = nodeFromDirect(ctx, 0);
    std::string key = dukIsStr(ctx, 1) ? duk_get_string(ctx, 1) : "";
    auto node = sharedNode(impl, raw);
    if (!node) { duk_push_undefined(ctx); return 1; }

    if (key == "tagName" || key == "nodeName") {
        duk_push_string(ctx, jbUpper(node->tag).c_str());
        return 1;
    }
    if (key == "id") {
        auto it = node->attrs.find("id");
        duk_push_string(ctx, it != node->attrs.end() ? it->second.c_str() : "");
        return 1;
    }
    if (key == "className") {
        auto it = node->attrs.find("class");
        duk_push_string(ctx, it != node->attrs.end() ? it->second.c_str() : "");
        return 1;
    }
    if (key == "textContent" || key == "innerText") {
        duk_push_string(ctx, concatText(node).c_str());
        return 1;
    }
    if (key == "innerHTML") {
        duk_push_string(ctx, serializeChildren(node).c_str());
        return 1;
    }
    if (key == "value") {
        auto it = node->attrs.find("value");
        duk_push_string(ctx, it != node->attrs.end() ? it->second.c_str() : "");
        return 1;
    }
    if (key == "checked") {
        duk_push_boolean(ctx, node->attrs.count("checked") > 0);
        return 1;
    }
    if (key == "href" || key == "src" || key == "type" || key == "name") {
        auto it = node->attrs.find(key);
        duk_push_string(ctx, it != node->attrs.end() ? it->second.c_str() : "");
        return 1;
    }
    if (key == "children" || key == "childNodes") {
        pushWrapperArray(impl, node->children);
        return 1;
    }
    // Tree-walking properties (round 3). Sibling lookups need the
    // parent's child list; null when the node is detached or at the end.
    if (key == "firstChild") {
        if (node->children.empty()) { duk_push_null(ctx); return 1; }
        impl->pushNodeWrapper(node->children.front());
        return 1;
    }
    if (key == "lastChild") {
        if (node->children.empty()) { duk_push_null(ctx); return 1; }
        impl->pushNodeWrapper(node->children.back());
        return 1;
    }
    if (key == "nextSibling" || key == "nextElementSibling" ||
        key == "previousSibling" || key == "previousElementSibling") {
        bool forward = (key == "nextSibling" || key == "nextElementSibling");
        bool elementOnly = (key == "nextElementSibling" ||
                            key == "previousElementSibling");
        auto p = node->parent.lock();
        if (!p) { duk_push_null(ctx); return 1; }
        const auto& kids = p->children;
        for (size_t i = 0; i < kids.size(); ++i) {
            if (kids[i].get() != node.get()) continue;
            auto step = [&](long d) -> std::shared_ptr<Node> {
                long j = (long)i;
                while (j + d >= 0 && j + d < (long)kids.size()) {
                    j += d;
                    if (!elementOnly || kids[j]->tag != "text")
                        return kids[j];
                }
                return nullptr;
            };
            auto sib = step(forward ? 1 : -1);
            if (sib) impl->pushNodeWrapper(sib); else duk_push_null(ctx);
            return 1;
        }
        duk_push_null(ctx);
        return 1;
    }
    if (key == "nodeType") {
        // Text nodes behave like DOM text (3); everything else is an
        // element (1). The document root reports 9 for completeness.
        int t = (node->tag == "text") ? 3
              : (node->tag == "root") ? 9 : 1;
        duk_push_int(ctx, t);
        return 1;
    }
    if (key == "offsetWidth" || key == "offsetHeight" ||
        key == "offsetTop"   || key == "offsetLeft") {
        // Border-box size / document-space position of the node's laid
        // out fragments (round 4). offsetTop/offsetLeft are relative to
        // the document rather than the offsetParent — this engine has a
        // single containing block, so the two coincide for every
        // practical page. Zeros when the node has no fragments.
        DOMRect r;
        bool ok = false;
        if (impl->self && impl->self->rectForNode)
            ok = impl->self->rectForNode(node, r);
        if (key == "offsetWidth")
            duk_push_number(ctx, ok ? (double)r.width  : 0.0);
        else if (key == "offsetHeight")
            duk_push_number(ctx, ok ? (double)r.height : 0.0);
        else if (key == "offsetTop")
            duk_push_number(ctx, ok ? (double)r.y      : 0.0);
        else
            duk_push_number(ctx, ok ? (double)r.x      : 0.0);
        return 1;
    }
    if (key == "classList") {
        duk_push_object(ctx);
        duk_push_pointer(ctx, (void*)node.get());
        duk_put_prop_string(ctx, -2, kNodePtr);
        duk_push_c_function(ctx, cl_add, DUK_VARARGS);
        duk_put_prop_string(ctx, -2, "add");
        duk_push_c_function(ctx, cl_remove, DUK_VARARGS);
        duk_put_prop_string(ctx, -2, "remove");
        duk_push_c_function(ctx, cl_toggle, DUK_VARARGS);
        duk_put_prop_string(ctx, -2, "toggle");
        duk_push_c_function(ctx, cl_contains, 1);
        duk_put_prop_string(ctx, -2, "contains");
        duk_push_c_function(ctx, cl_item, 1);
        duk_put_prop_string(ctx, -2, "item");
        duk_push_c_function(ctx, cl_value, 0);
        duk_put_prop_string(ctx, -2, "value");
        duk_push_string(ctx, "length");
        duk_push_c_function(ctx, cl_length, 0);
        duk_def_prop(ctx, -3, DUK_DEFPROP_HAVE_GETTER | DUK_DEFPROP_FORCE);
        return 1;
    }
    if (key == "parentNode" || key == "parentElement") {
        auto p = node->parent.lock();
        if (p) impl->pushNodeWrapper(p); else duk_push_null(ctx);
        return 1;
    }
    if (key == "style") {
        // style target object carrying a hidden Node*
        duk_push_object(ctx);
        duk_push_pointer(ctx, (void*)node.get());
        duk_put_prop_string(ctx, -2, kStylePtr);
        duk_get_global_string(ctx, "__makeStyle");
        duk_dup(ctx, -2);
        duk_call(ctx, 1);
        duk_remove(ctx, -2);   // drop target, leave Proxy
        return 1;
    }
    if (key == "disabled") {
        duk_push_boolean(ctx, node->attrs.count("disabled") > 0);
        return 1;
    }
    // HTMLMediaElement-lite: <video>/<audio> properties backed by the
    // internal player registry (round 9). Missing player = defaults,
    // like a media element before it has loaded anything.
    if (node->tag == "video" || node->tag == "audio") {
        if (key == "paused" || key == "ended" || key == "duration" ||
            key == "currentTime" || key == "muted" || key == "volume") {
            auto p = media::findPlayer(mediaSourceUrlFor(impl, node));
            if (key == "paused") {
                duk_push_boolean(ctx, p ? p->paused() : true);
            } else if (key == "ended") {
                duk_push_boolean(ctx, p ? p->ended() : false);
            } else if (key == "duration") {
                duk_push_number(ctx, p ? p->duration() : 0.0);
            } else if (key == "currentTime") {
                duk_push_number(ctx, p ? p->position() : 0.0);
            } else if (key == "muted") {
                duk_push_boolean(ctx, p ? p->muted() : false);
            } else {
                duk_push_number(ctx, p ? p->volume() : 1.0);
            }
            return 1;
        }
    }
    // Fallback: normal lookup on the target (its proto has the methods).
    // NOTE: duk_get_prop takes the key from the stack top — we must push
    // the key explicitly. Using arg2 (the receiver proxy) as the key made
    // ToString(receiver) re-enter this trap infinitely.
    duk_dup(ctx, 1);
    duk_get_prop(ctx, 0);
    return 1;
}

duk_ret_t nodeHandlerSet(duk_context* ctx) {
    JSEngine::Impl* impl = getImpl(ctx);
    Node* raw = nodeFromDirect(ctx, 0);
    std::string key = dukIsStr(ctx, 1) ? duk_get_string(ctx, 1) : "";
    auto node = sharedNode(impl, raw);
    if (!node) { duk_push_false(ctx); return 1; }

    if (key == "id" || key == "className" || key == "value" ||
        key == "href" || key == "src" || key == "type" || key == "name") {
        std::string attr = (key == "className") ? "class" : key;
        std::string v = dukToStdString(ctx, 2);
        if (v.empty()) node->attrs.erase(attr);
        else           node->attrs[attr] = v;
        impl->notifyMutation();
        duk_push_true(ctx);
        return 1;
    }
    if (key == "checked") {
        if (duk_get_boolean(ctx, 2)) node->attrs["checked"] = "checked";
        else                         node->attrs.erase("checked");
        impl->notifyMutation();
        duk_push_true(ctx);
        return 1;
    }
    // Media element writes: currentTime seeks, muted/volume/loop map onto
    // the internal player (no-ops when the page has not laid out yet).
    if ((node->tag == "video" || node->tag == "audio") &&
        (key == "currentTime" || key == "muted" || key == "volume" ||
         key == "loop")) {
        auto p = media::findPlayer(mediaSourceUrlFor(impl, node));
        if (p) {
            if (key == "currentTime")      p->seek(duk_get_number(ctx, 2));
            else if (key == "muted")       p->setMuted(duk_get_boolean(ctx, 2) != 0);
            else if (key == "volume")      p->setVolume((float)duk_get_number(ctx, 2));
            else                            p->setLoop(duk_get_boolean(ctx, 2) != 0);
        }
        duk_push_true(ctx);
        return 1;
    }
    if (key == "textContent" || key == "innerText") {
        std::string v = dukToStdString(ctx, 2);
        node->children.clear();
        if (!v.empty()) {
            auto t = std::make_shared<Node>();
            t->tag = "text";
            t->text = v;
            t->parent = node;
            node->children.push_back(t);
        }
        impl->notifyMutation();
        duk_push_true(ctx);
        return 1;
    }
    if (key == "innerHTML") {
        std::string v = dukToStdString(ctx, 2);
        auto parsed = parseHTML(v);
        node->children.clear();
        for (auto& c : parsed->children) {
            c->parent = node;
            node->children.push_back(c);
        }
        impl->notifyMutation();
        duk_push_true(ctx);
        return 1;
    }
    if (key == "style") {
        std::string v = dukToStdString(ctx, 2);
        if (v.empty()) node->attrs.erase("style");
        else           node->attrs["style"] = v;
        impl->notifyMutation();
        duk_push_true(ctx);
        return 1;
    }
    // Plain JS property on the target object.
    duk_dup(ctx, 1);   // key
    duk_dup(ctx, 2);   // value
    duk_put_prop(ctx, 0);
    duk_push_true(ctx);
    return 1;
}

// ---------------------------------------------------------------------------
// Style object traps
// ---------------------------------------------------------------------------

static Node* styleNodeFromTarget(duk_context* ctx, duk_idx_t idx) {
    duk_get_prop_string(ctx, idx, kStylePtr);
    if (!duk_is_pointer(ctx, -1)) { duk_pop(ctx); return nullptr; }
    void* p = duk_get_pointer(ctx, -1);
    duk_pop(ctx);
    return static_cast<Node*>(p);
}

duk_ret_t styleHandlerGet(duk_context* ctx) {
    JSEngine::Impl* impl = getImpl(ctx);
    auto node = sharedNode(impl, styleNodeFromTarget(ctx, 0));
    std::string key = dukIsStr(ctx, 1) ? duk_get_string(ctx, 1) : "";

    if (key == "cssText") {
        std::string s = node && node->attrs.count("style") ? node->attrs["style"] : "";
        duk_push_string(ctx, s.c_str());
        return 1;
    }
    if (node) {
        std::string v = getNodeStyle(node, key);
        if (!v.empty()) { duk_push_string(ctx, v.c_str()); return 1; }
    }
    duk_dup(ctx, 1);   // key (see note in nodeHandlerGet)
    duk_get_prop(ctx, 0);
    return 1;
}

duk_ret_t styleHandlerSet(duk_context* ctx) {
    JSEngine::Impl* impl = getImpl(ctx);
    auto node = sharedNode(impl, styleNodeFromTarget(ctx, 0));
    std::string key = dukIsStr(ctx, 1) ? duk_get_string(ctx, 1) : "";
    std::string val = dukToStdString(ctx, 2);
    if (node && !key.empty()) {
        setNodeStyle(node, key, val);
        impl->notifyMutation();
    }
    duk_push_true(ctx);
    return 1;
}

// ---------------------------------------------------------------------------
// Node methods (live on the wrapper target's prototype)
// ---------------------------------------------------------------------------

// el.getBoundingClientRect() -> DOMRect (round 4).
// Viewport-relative union of the element's laid-out fragments, exactly
// like the DOM spec: coordinates are client coordinates (document space
// minus the scroll offset), width/height are the fragment-union extents,
// and top/right/bottom/left mirror the edges. A node with no laid-out
// fragments (detached, display:none, not yet in the document) returns an
// all-zero rect — real browsers do the same for display:none.
duk_ret_t nm_getBoundingClientRect(duk_context* ctx) {
    JSEngine::Impl* impl = getImpl(ctx);
    auto node = sharedNode(impl, thisNodeRaw(ctx));

    DOMRect r;   // all zeros unless the host has geometry for this node
    if (node && impl->self && impl->self->rectForNode)
        impl->self->rectForNode(node, r);
    if (impl->self && impl->self->scrollY)
        r.y -= (float)impl->self->scrollY();

    duk_push_object(ctx);
    duk_push_number(ctx, (double)r.x);          duk_put_prop_string(ctx, -2, "x");
    duk_push_number(ctx, (double)r.y);          duk_put_prop_string(ctx, -2, "y");
    duk_push_number(ctx, (double)r.width);      duk_put_prop_string(ctx, -2, "width");
    duk_push_number(ctx, (double)r.height);     duk_put_prop_string(ctx, -2, "height");
    duk_push_number(ctx, (double)r.left());     duk_put_prop_string(ctx, -2, "left");
    duk_push_number(ctx, (double)r.top());      duk_put_prop_string(ctx, -2, "top");
    duk_push_number(ctx, (double)r.right());    duk_put_prop_string(ctx, -2, "right");
    duk_push_number(ctx, (double)r.bottom());   duk_put_prop_string(ctx, -2, "bottom");
    return 1;
}

duk_ret_t nm_appendChild(duk_context* ctx) {
    JSEngine::Impl* impl = getImpl(ctx);
    auto parent = sharedNode(impl, thisNodeRaw(ctx));
    if (!parent) { duk_push_undefined(ctx); return 1; }
    duk_get_prop_string(ctx, 0, kNodePtr);
    Node* rawChild = duk_is_pointer(ctx, -1)
        ? static_cast<Node*>(duk_get_pointer(ctx, -1)) : nullptr;
    duk_pop(ctx);
    auto child = sharedNode(impl, rawChild);
    if (!child) { duk_push_undefined(ctx); return 1; }
    child->parent = parent;
    parent->children.push_back(child);
    impl->notifyMutation();
    duk_dup(ctx, 0);   // return the child
    return 1;
}

duk_ret_t nm_removeChild(duk_context* ctx) {
    JSEngine::Impl* impl = getImpl(ctx);
    auto parent = sharedNode(impl, thisNodeRaw(ctx));
    if (!parent) { duk_push_undefined(ctx); return 1; }
    duk_get_prop_string(ctx, 0, kNodePtr);
    Node* rawChild = duk_is_pointer(ctx, -1)
        ? static_cast<Node*>(duk_get_pointer(ctx, -1)) : nullptr;
    duk_pop(ctx);
    auto& kids = parent->children;
    for (auto it = kids.begin(); it != kids.end(); ++it) {
        if (it->get() == rawChild) {
            kids.erase(it);
            impl->notifyMutation();
            duk_dup(ctx, 0);
            return 1;
        }
    }
    duk_push_undefined(ctx);
    return 1;
}

duk_ret_t nm_remove(duk_context* ctx) {
    JSEngine::Impl* impl = getImpl(ctx);
    auto node = sharedNode(impl, thisNodeRaw(ctx));
    if (!node) return 0;
    auto parent = node->parent.lock();
    if (parent) {
        auto& kids = parent->children;
        for (auto it = kids.begin(); it != kids.end(); ++it) {
            if (it->get() == node.get()) {
                kids.erase(it);
                impl->notifyMutation();
                break;
            }
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Tree-manipulation extras (round 3): insertBefore, replaceChild,
// cloneNode, contains, closest, matches. Pages written against the real
// DOM use these constantly (menus, tab widgets, template stamping).
// ---------------------------------------------------------------------------

// Read the hidden Node* from a wrapper-valued argument (may be null).
static Node* nodeFromArg(duk_context* ctx, duk_idx_t idx) {
    if (duk_is_null_or_undefined(ctx, idx) || !duk_is_object(ctx, idx))
        return nullptr;
    duk_get_prop_string(ctx, idx, kNodePtr);
    Node* p = duk_is_pointer(ctx, -1)
        ? static_cast<Node*>(duk_get_pointer(ctx, -1)) : nullptr;
    duk_pop(ctx);
    return p;
}

duk_ret_t nm_insertBefore(duk_context* ctx) {
    JSEngine::Impl* impl = getImpl(ctx);
    auto parent = sharedNode(impl, thisNodeRaw(ctx));
    if (!parent) { duk_push_undefined(ctx); return 1; }
    auto child = sharedNode(impl, nodeFromArg(ctx, 0));
    if (!child) { duk_push_undefined(ctx); return 1; }
    Node* rawRef = nodeFromArg(ctx, 1);

    child->parent = parent;
    auto& kids = parent->children;
    if (rawRef) {
        for (auto it = kids.begin(); it != kids.end(); ++it) {
            if (it->get() == rawRef) {
                kids.insert(it, child);
                impl->notifyMutation();
                duk_dup(ctx, 0);
                return 1;
            }
        }
    }
    kids.push_back(child);   // ref missing/null -> behaves like appendChild
    impl->notifyMutation();
    duk_dup(ctx, 0);
    return 1;
}

duk_ret_t nm_replaceChild(duk_context* ctx) {
    JSEngine::Impl* impl = getImpl(ctx);
    auto parent = sharedNode(impl, thisNodeRaw(ctx));
    if (!parent) { duk_push_undefined(ctx); return 1; }
    auto newChild = sharedNode(impl, nodeFromArg(ctx, 0));
    Node* rawOld = nodeFromArg(ctx, 1);
    if (!newChild || !rawOld) { duk_push_undefined(ctx); return 1; }

    auto& kids = parent->children;
    for (auto it = kids.begin(); it != kids.end(); ++it) {
        if (it->get() == rawOld) {
            newChild->parent = parent;
            *it = newChild;
            impl->notifyMutation();
            duk_dup(ctx, 0);
            return 1;
        }
    }
    duk_push_undefined(ctx);
    return 1;
}

static std::shared_ptr<Node> deepClone(const std::shared_ptr<Node>& n,
                                       bool deep) {
    auto c = std::make_shared<Node>();
    c->tag = n->tag;
    c->text = n->text;
    c->attrs = n->attrs;
    if (deep) {
        for (auto& ch : n->children) {
            auto cc = deepClone(ch, deep);
            cc->parent = c;
            c->children.push_back(cc);
        }
    }
    return c;
}

duk_ret_t nm_cloneNode(duk_context* ctx) {
    JSEngine::Impl* impl = getImpl(ctx);
    auto node = sharedNode(impl, thisNodeRaw(ctx));
    if (!node) { duk_push_null(ctx); return 1; }
    bool deep = duk_get_top(ctx) >= 1 && dukIsTruthy(ctx, 0);
    // The clone's parent is deliberately null (matches the DOM spec):
    // it must be insertBefore/appendChild'd before it is on the page.
    auto clone = deepClone(node, deep);
    impl->alive[clone.get()] = clone;   // keep the clone wrapper-alive
    impl->pushNodeWrapper(clone);
    return 1;
}

static bool subtreeContains(const std::shared_ptr<Node>& root, Node* raw) {
    if (!root) return false;
    if (root.get() == raw) return true;
    for (auto& c : root->children)
        if (subtreeContains(c, raw)) return true;
    return false;
}

duk_ret_t nm_contains(duk_context* ctx) {
    JSEngine::Impl* impl = getImpl(ctx);
    auto node = sharedNode(impl, thisNodeRaw(ctx));
    Node* rawOther = nodeFromArg(ctx, 0);
    duk_push_boolean(ctx, node && rawOther && subtreeContains(node, rawOther));
    return 1;
}

duk_ret_t nm_closest(duk_context* ctx) {
    JSEngine::Impl* impl = getImpl(ctx);
    auto node = sharedNode(impl, thisNodeRaw(ctx));
    if (!node || duk_get_top(ctx) < 1) { duk_push_null(ctx); return 1; }
    std::string sel = dukToStdString(ctx, 0);
    for (auto n = node; n; n = n->parent.lock()) {
        if (simpleSelectorMatches(sel, n)) {
            impl->pushNodeWrapper(n);
            return 1;
        }
    }
    duk_push_null(ctx);
    return 1;
}

duk_ret_t nm_matches(duk_context* ctx) {
    JSEngine::Impl* impl = getImpl(ctx);
    auto node = sharedNode(impl, thisNodeRaw(ctx));
    if (!node || duk_get_top(ctx) < 1) { duk_push_false(ctx); return 1; }
    duk_push_boolean(ctx, simpleSelectorMatches(dukToStdString(ctx, 0), node));
    return 1;
}

// ---------------------------------------------------------------------------
// classList — the DOMTokenList-shaped helper every modern page touches.
// Backed by the node's class attribute; mutations notify the host so the
// page relayouts, exactly like `className = ...` does.
// ---------------------------------------------------------------------------

static std::string classListOf(const std::shared_ptr<Node>& n) {
    auto it = n->attrs.find("class");
    return it == n->attrs.end() ? "" : it->second;
}

static void classListSplit(const std::string& cls,
                           std::vector<std::string>& out) {
    std::istringstream ss(cls);
    std::string c;
    while (ss >> c) out.push_back(c);
}

static bool classListHas(const std::string& cls, const std::string& name) {
    std::vector<std::string> toks;
    classListSplit(cls, toks);
    return std::find(toks.begin(), toks.end(), name) != toks.end();
}

static void classListAssign(JSEngine::Impl* impl,
                            const std::shared_ptr<Node>& n,
                            const std::vector<std::string>& toks) {
    std::string joined;
    for (size_t i = 0; i < toks.size(); ++i) {
        if (i) joined += ' ';
        joined += toks[i];
    }
    if (joined.empty()) n->attrs.erase("class");
    else                n->attrs["class"] = joined;
    impl->notifyMutation();
}

// classList methods receive the classList object as `this`; the hidden
// Node* rides on it (same pattern as the style proxy).
static std::shared_ptr<Node> classListNode(duk_context* ctx) {
    JSEngine::Impl* impl = getImpl(ctx);
    duk_push_this(ctx);
    duk_get_prop_string(ctx, -1, kNodePtr);
    Node* raw = duk_is_pointer(ctx, -1)
        ? static_cast<Node*>(duk_get_pointer(ctx, -1)) : nullptr;
    duk_pop(ctx);
    duk_pop(ctx);   // this
    return sharedNode(impl, raw);
}

duk_ret_t cl_add(duk_context* ctx) {
    JSEngine::Impl* impl = getImpl(ctx);
    auto node = classListNode(ctx);
    if (!node) return 0;
    std::vector<std::string> toks;
    classListSplit(classListOf(node), toks);
    for (duk_idx_t i = 0; i < duk_get_top(ctx); ++i) {
        std::string name = dukToStdString(ctx, i);
        if (name.empty()) continue;
        if (std::find(toks.begin(), toks.end(), name) == toks.end())
            toks.push_back(name);
    }
    classListAssign(impl, node, toks);
    return 0;
}

duk_ret_t cl_remove(duk_context* ctx) {
    JSEngine::Impl* impl = getImpl(ctx);
    auto node = classListNode(ctx);
    if (!node) return 0;
    std::vector<std::string> toks;
    classListSplit(classListOf(node), toks);
    for (duk_idx_t i = 0; i < duk_get_top(ctx); ++i) {
        std::string name = dukToStdString(ctx, i);
        toks.erase(std::remove(toks.begin(), toks.end(), name), toks.end());
    }
    classListAssign(impl, node, toks);
    return 0;
}

duk_ret_t cl_toggle(duk_context* ctx) {
    JSEngine::Impl* impl = getImpl(ctx);
    auto node = classListNode(ctx);
    if (!node || duk_get_top(ctx) < 1) { duk_push_false(ctx); return 1; }
    std::string name = dukToStdString(ctx, 0);
    bool has = classListHas(classListOf(node), name);
    // toggle(name) flips; toggle(name, force) sets to the given state.
    bool want = (duk_get_top(ctx) >= 2) ? dukIsTruthy(ctx, 1) : !has;
    std::vector<std::string> toks;
    classListSplit(classListOf(node), toks);
    if (want && !has) toks.push_back(name);
    if (!want && has) toks.erase(std::remove(toks.begin(), toks.end(), name),
                                 toks.end());
    classListAssign(impl, node, toks);
    duk_push_boolean(ctx, want);
    return 1;
}

duk_ret_t cl_contains(duk_context* ctx) {
    auto node = classListNode(ctx);
    if (!node || duk_get_top(ctx) < 1) { duk_push_false(ctx); return 1; }
    duk_push_boolean(ctx, classListHas(classListOf(node),
                                       dukToStdString(ctx, 0)));
    return 1;
}

duk_ret_t cl_item(duk_context* ctx) {
    auto node = classListNode(ctx);
    if (!node || duk_get_top(ctx) < 1 || !duk_is_number(ctx, 0)) {
        duk_push_null(ctx); return 1;
    }
    std::vector<std::string> toks;
    classListSplit(classListOf(node), toks);
    long i = (long)duk_get_int(ctx, 0);
    if (i < 0 || i >= (long)toks.size()) { duk_push_null(ctx); return 1; }
    duk_push_string(ctx, toks[i].c_str());
    return 1;
}

duk_ret_t cl_length(duk_context* ctx) {
    auto node = classListNode(ctx);
    if (!node) { duk_push_int(ctx, 0); return 1; }
    std::vector<std::string> toks;
    classListSplit(classListOf(node), toks);
    duk_push_int(ctx, (int)toks.size());
    return 1;
}

duk_ret_t cl_value(duk_context* ctx) {
    auto node = classListNode(ctx);
    duk_push_string(ctx, node ? classListOf(node).c_str() : "");
    return 1;
}

duk_ret_t nm_setAttribute(duk_context* ctx) {
    JSEngine::Impl* impl = getImpl(ctx);
    auto node = sharedNode(impl, thisNodeRaw(ctx));
    if (node) {
        std::string k = jbLower(dukToStdString(ctx, 0));
        std::string v = dukToStdString(ctx, 1);
        if (!k.empty()) {
            node->attrs[k] = v;
            impl->notifyMutation();
        }
    }
    return 0;
}

duk_ret_t nm_getAttribute(duk_context* ctx) {
    JSEngine::Impl* impl = getImpl(ctx);
    auto node = sharedNode(impl, thisNodeRaw(ctx));
    std::string k = jbLower(dukToStdString(ctx, 0));
    if (node) {
        auto it = node->attrs.find(k);
        if (it != node->attrs.end()) {
            duk_push_string(ctx, it->second.c_str());
            return 1;
        }
    }
    duk_push_null(ctx);
    return 1;
}

duk_ret_t nm_removeAttribute(duk_context* ctx) {
    JSEngine::Impl* impl = getImpl(ctx);
    auto node = sharedNode(impl, thisNodeRaw(ctx));
    if (node) {
        std::string k = jbLower(dukToStdString(ctx, 0));
        if (node->attrs.erase(k) > 0) impl->notifyMutation();
    }
    return 0;
}

duk_ret_t nm_hasAttribute(duk_context* ctx) {
    JSEngine::Impl* impl = getImpl(ctx);
    auto node = sharedNode(impl, thisNodeRaw(ctx));
    std::string k = jbLower(dukToStdString(ctx, 0));
    duk_push_boolean(ctx, node && node->attrs.count(k) > 0);
    return 1;
}

duk_ret_t nm_addEventListener(duk_context* ctx) {
    JSEngine::Impl* impl = getImpl(ctx);
    Node* raw = thisNodeRaw(ctx);
    auto node = sharedNode(impl, raw);
    if (!node || !duk_is_function(ctx, 1)) return 0;
    std::string type = dukToStdString(ctx, 0);
    if (type.empty()) return 0;
    int id = impl->stashFunction(1);
    impl->listeners[{raw, type}].push_back(id);
    return 0;
}

duk_ret_t nm_removeEventListener(duk_context* ctx) {
    JSEngine::Impl* impl = getImpl(ctx);
    Node* raw = thisNodeRaw(ctx);
    if (!raw) return 0;
    std::string type = dukToStdString(ctx, 0);
    auto it = impl->listeners.find({raw, type});
    if (it == impl->listeners.end()) return 0;
    // Remove the LAST registered handler (LIFO-ish, close enough).
    if (!it->second.empty()) it->second.pop_back();
    return 0;
}

duk_ret_t nm_click(duk_context* ctx) {
    JSEngine::Impl* impl = getImpl(ctx);
    auto node = sharedNode(impl, thisNodeRaw(ctx));
    if (node && impl->self) impl->self->dispatchEvent("click", node);
    return 0;
}

duk_ret_t nm_focus(duk_context* ctx) {
    JSEngine::Impl* impl = getImpl(ctx);
    auto node = sharedNode(impl, thisNodeRaw(ctx));
    if (node && impl->self && impl->self->onFocusNode)
        impl->self->onFocusNode(node);
    return 0;
}

duk_ret_t nm_querySelector(duk_context* ctx) {
    JSEngine::Impl* impl = getImpl(ctx);
    auto node = sharedNode(impl, thisNodeRaw(ctx));
    if (!node) { duk_push_null(ctx); return 1; }
    std::string sel = dukToStdString(ctx, 0);
    std::vector<std::shared_ptr<Node>> out;
    walkSelector(sel, node, out);
    if (out.empty()) { duk_push_null(ctx); return 1; }
    impl->pushNodeWrapper(out.front());
    return 1;
}

duk_ret_t nm_querySelectorAll(duk_context* ctx) {
    JSEngine::Impl* impl = getImpl(ctx);
    auto node = sharedNode(impl, thisNodeRaw(ctx));
    std::string sel = dukToStdString(ctx, 0);
    std::vector<std::shared_ptr<Node>> out;
    if (node) walkSelector(sel, node, out);
    pushWrapperArray(impl, out);
    return 1;
}

// ---------------------------------------------------------------------------
// document bindings
// ---------------------------------------------------------------------------

std::shared_ptr<Node> findFirstByTag(const std::shared_ptr<Node>& root,
                                     const std::string& tag) {
    if (!root) return nullptr;
    for (auto& c : root->children) {
        if (c->tag == tag) return c;
        auto deep = findFirstByTag(c, tag);
        if (deep) return deep;
    }
    return nullptr;
}

duk_ret_t doc_getElementById(duk_context* ctx) {
    JSEngine::Impl* impl = getImpl(ctx);
    std::string id = dukToStdString(ctx, 0);
    auto n = findById(impl->documentRoot, id);
    if (n) impl->pushNodeWrapper(n);
    else   duk_push_null(ctx);
    return 1;
}

duk_ret_t doc_querySelector(duk_context* ctx) {
    JSEngine::Impl* impl = getImpl(ctx);
    std::string sel = dukToStdString(ctx, 0);
    std::vector<std::shared_ptr<Node>> out;
    if (impl->documentRoot) walkSelector(sel, impl->documentRoot, out);
    if (out.empty()) { duk_push_null(ctx); return 1; }
    impl->pushNodeWrapper(out.front());
    return 1;
}

duk_ret_t doc_querySelectorAll(duk_context* ctx) {
    JSEngine::Impl* impl = getImpl(ctx);
    std::string sel = dukToStdString(ctx, 0);
    std::vector<std::shared_ptr<Node>> out;
    if (impl->documentRoot) walkSelector(sel, impl->documentRoot, out);
    pushWrapperArray(impl, out);
    return 1;
}

duk_ret_t doc_createElement(duk_context* ctx) {
    JSEngine::Impl* impl = getImpl(ctx);
    std::string tag = jbLower(dukToStdString(ctx, 0));
    auto n = std::make_shared<Node>();
    n->tag = tag;
    impl->pushNodeWrapper(n);
    return 1;
}

duk_ret_t doc_createTextNode(duk_context* ctx) {
    JSEngine::Impl* impl = getImpl(ctx);
    auto n = std::make_shared<Node>();
    n->tag = "text";
    n->text = dukToStdString(ctx, 0);
    impl->pushNodeWrapper(n);
    return 1;
}

duk_ret_t doc_addEventListener(duk_context* ctx) {
    JSEngine::Impl* impl = getImpl(ctx);
    if (!duk_is_function(ctx, 1)) return 0;
    std::string type = dukToStdString(ctx, 0);
    int id = impl->stashFunction(1);
    impl->docListeners[type].push_back(id);
    return 0;
}

duk_ret_t doc_getBody(duk_context* ctx) {
    JSEngine::Impl* impl = getImpl(ctx);
    auto b = findFirstByTag(impl->documentRoot, "body");
    if (b) impl->pushNodeWrapper(b);
    else   duk_push_null(ctx);
    return 1;
}

duk_ret_t doc_getDocumentElement(duk_context* ctx) {
    JSEngine::Impl* impl = getImpl(ctx);
    auto h = findFirstByTag(impl->documentRoot, "html");
    if (!h) h = impl->documentRoot;
    if (h) impl->pushNodeWrapper(h);
    else   duk_push_null(ctx);
    return 1;
}

duk_ret_t doc_getTitle(duk_context* ctx) {
    JSEngine::Impl* impl = getImpl(ctx);
    auto t = findFirstByTag(impl->documentRoot, "title");
    std::string s = t ? concatText(t) : "";
    duk_push_string(ctx, jbTrim(s).c_str());
    return 1;
}

duk_ret_t doc_setTitle(duk_context* ctx) {
    JSEngine::Impl* impl = getImpl(ctx);
    auto t = findFirstByTag(impl->documentRoot, "title");
    if (!t) {
        auto head = findFirstByTag(impl->documentRoot, "head");
        t = std::make_shared<Node>();
        t->tag = "title";
        t->parent = head ? head : impl->documentRoot;
        (head ? head : impl->documentRoot)->children.push_back(t);
    }
    t->children.clear();
    auto txt = std::make_shared<Node>();
    txt->tag = "text";
    txt->text = dukToStdString(ctx, 0);
    txt->parent = t;
    t->children.push_back(txt);
    impl->notifyMutation();
    return 0;
}

// ---------------------------------------------------------------------------
// console / alert / confirm
// ---------------------------------------------------------------------------

duk_ret_t console_log(duk_context* ctx) {
    std::string line;
    int n = duk_get_top(ctx);
    for (int i = 0; i < n; ++i) {
        if (i) line += " ";
        line += dukToStdString(ctx, i);
    }
    std::cout << "[console] " << line << std::endl;
    return 0;
}

duk_ret_t console_err(duk_context* ctx) {
    std::string line;
    int n = duk_get_top(ctx);
    for (int i = 0; i < n; ++i) {
        if (i) line += " ";
        line += dukToStdString(ctx, i);
    }
    std::cerr << "[console.error] " << line << std::endl;
    return 0;
}

duk_ret_t js_alert(duk_context* ctx) {
    JSEngine::Impl* impl = getImpl(ctx);
    std::string msg;
    if (duk_get_top(ctx) > 0) msg = dukToStdString(ctx, 0);
    if (impl->self) {
        if (impl->self->onAlert) impl->self->onAlert(msg);
        else std::cout << "[alert] " << msg << std::endl;
    }
    return 0;
}

duk_ret_t js_confirm(duk_context* ctx) {
    JSEngine::Impl* impl = getImpl(ctx);
    std::string msg;
    if (duk_get_top(ctx) > 0) msg = dukToStdString(ctx, 0);
    bool ok = false;
    if (impl->self && impl->self->onConfirm) ok = impl->self->onConfirm(msg);
    duk_push_boolean(ctx, ok);
    return 1;
}

// ---------------------------------------------------------------------------
// Timers
// ---------------------------------------------------------------------------

int makeTimer(JSEngine::Impl* impl, int delayMs, bool repeating) {
    JSTimer t;
    t.id = impl->nextTimerId++;
    t.intervalMs = std::max(0, delayMs);
    t.repeating = repeating;
    t.fireAt = std::chrono::steady_clock::now() +
               std::chrono::milliseconds(std::max(0, delayMs));
    impl->timers.push_back(std::move(t));
    return impl->timers.back().id;
}

duk_ret_t js_setTimeout(duk_context* ctx) {
    JSEngine::Impl* impl = getImpl(ctx);
    int delay = 0;
    if (duk_get_top(ctx) >= 2 && duk_is_number(ctx, 1))
        delay = (int)duk_get_int(ctx, 1);
    int id = makeTimer(impl, delay, false);
    JSTimer& t = impl->timers.back();
    if (duk_is_function(ctx, 0)) {
        t.isFn = true;
        t.fnStash = impl->stashFunction(0);
    } else {
        t.isFn = false;
        t.code = dukToStdString(ctx, 0);
    }
    duk_push_int(ctx, id);
    return 1;
}

duk_ret_t js_setInterval(duk_context* ctx) {
    JSEngine::Impl* impl = getImpl(ctx);
    int delay = 1000;
    if (duk_get_top(ctx) >= 2 && duk_is_number(ctx, 1))
        delay = (int)duk_get_int(ctx, 1);
    int id = makeTimer(impl, delay, true);
    JSTimer& t = impl->timers.back();
    if (duk_is_function(ctx, 0)) {
        t.isFn = true;
        t.fnStash = impl->stashFunction(0);
    } else {
        t.isFn = false;
        t.code = dukToStdString(ctx, 0);
    }
    duk_push_int(ctx, id);
    return 1;
}

duk_ret_t js_clearTimer(duk_context* ctx) {
    JSEngine::Impl* impl = getImpl(ctx);
    if (duk_get_top(ctx) < 1 || !duk_is_number(ctx, 0)) return 0;
    int id = (int)duk_get_int(ctx, 0);
    for (auto& t : impl->timers)
        if (t.id == id) t.cancelled = true;
    return 0;
}

// ---------------------------------------------------------------------------
// fetch + XMLHttpRequest (both synchronous — this browser runs its JS on
// the UI thread; an async fetch would need a real event loop)
// ---------------------------------------------------------------------------

duk_ret_t js_fetch_json(duk_context* ctx) {
    duk_push_this(ctx);
    duk_get_prop_string(ctx, -1, "text");
    if (!duk_is_string(ctx, -1)) {
        duk_type_error(ctx, "response has no text");
        return 1;
    }
    duk_json_decode(ctx, -1);   // decode the string in place
    duk_remove(ctx, -2);        // drop `this`, leave the decoded value
    return 1;
}

void pushFetchObject(JSEngine::Impl* impl, const FetchResult& fr) {
    duk_context* ctx = impl->ctx;
    duk_push_object(ctx);
    duk_push_boolean(ctx, fr.ok);
    duk_put_prop_string(ctx, -2, "ok");
    duk_push_int(ctx, (int)fr.status);
    duk_put_prop_string(ctx, -2, "status");
    duk_push_string(ctx, fr.body.c_str());
    duk_put_prop_string(ctx, -2, "text");
    duk_push_string(ctx, fr.finalUrl.c_str());
    duk_put_prop_string(ctx, -2, "url");
    duk_push_string(ctx, fr.contentType.c_str());
    duk_put_prop_string(ctx, -2, "contentType");
    duk_push_string(ctx, fr.error.c_str());
    duk_put_prop_string(ctx, -2, "error");
    duk_push_c_function(ctx, js_fetch_json, 0);
    duk_put_prop_string(ctx, -2, "json");
}

duk_ret_t js_fetch(duk_context* ctx) {
    JSEngine::Impl* impl = getImpl(ctx);
    std::string url = (duk_get_top(ctx) >= 1) ? dukToStdString(ctx, 0) : "";
    std::string full = impl->resolveUrl(url);
    FetchResult fr = fetchUrlCached(full);
    pushFetchObject(impl, fr);
    return 1;
}

duk_ret_t xhr_open(duk_context* ctx) {
    JSEngine::Impl* impl = getImpl(ctx);
    (void)impl;
    duk_push_this(ctx);
    std::string method = (duk_get_top(ctx) >= 2) ? jbUpper(dukToStdString(ctx, 0)) : "GET";
    std::string url    = (duk_get_top(ctx) >= 2) ? dukToStdString(ctx, 1) : "";
    duk_push_string(ctx, method.c_str());
    duk_put_prop_string(ctx, -2, "_method");
    duk_push_string(ctx, url.c_str());
    duk_put_prop_string(ctx, -2, "_url");
    duk_push_int(ctx, 1);
    duk_put_prop_string(ctx, -2, "readyState");
    return 0;
}

duk_ret_t xhr_send(duk_context* ctx) {
    JSEngine::Impl* impl = getImpl(ctx);
    duk_push_this(ctx);
    duk_idx_t selfIdx = duk_normalize_index(ctx, -1);
    duk_get_prop_string(ctx, selfIdx, "_url");
    std::string url = duk_is_string(ctx, -1) ? duk_get_string(ctx, -1) : "";
    duk_pop(ctx);
    std::string full = impl->resolveUrl(url);
    FetchResult fr = fetchUrlCached(full);
    duk_push_int(ctx, 4);
    duk_put_prop_string(ctx, selfIdx, "readyState");
    duk_push_int(ctx, (int)fr.status);
    duk_put_prop_string(ctx, selfIdx, "status");
    duk_push_string(ctx, fr.ok ? "OK" : (fr.error.empty() ? "Error" : fr.error.c_str()));
    duk_put_prop_string(ctx, selfIdx, "statusText");
    duk_push_string(ctx, fr.body.c_str());
    duk_put_prop_string(ctx, selfIdx, "responseText");
    duk_push_string(ctx, fr.body.c_str());
    duk_put_prop_string(ctx, selfIdx, "response");
    return 0;
}

duk_ret_t js_XMLHttpRequest(duk_context* ctx) {
    // Constructor: `new XMLHttpRequest()`. `this` is the fresh object
    // when called with duk_new.
    duk_push_int(ctx, 0);
    duk_put_prop_string(ctx, 0, "readyState");
    duk_push_int(ctx, 0);
    duk_put_prop_string(ctx, 0, "status");
    duk_push_string(ctx, "");
    duk_put_prop_string(ctx, 0, "responseText");
    duk_push_string(ctx, "");
    duk_put_prop_string(ctx, 0, "response");
    duk_push_string(ctx, "");
    duk_put_prop_string(ctx, 0, "_method");
    duk_push_string(ctx, "");
    duk_put_prop_string(ctx, 0, "_url");
    duk_push_c_function(ctx, xhr_open, 2);
    duk_put_prop_string(ctx, 0, "open");
    duk_push_c_function(ctx, xhr_send, 0);
    duk_put_prop_string(ctx, 0, "send");
    return 0;
}

// ---------------------------------------------------------------------------
// location
// ---------------------------------------------------------------------------

duk_ret_t loc_href_get(duk_context* ctx) {
    JSEngine::Impl* impl = getImpl(ctx);
    duk_push_string(ctx, impl->documentUrl.c_str());
    return 1;
}

duk_ret_t loc_href_set(duk_context* ctx) {
    JSEngine::Impl* impl = getImpl(ctx);
    std::string u = dukToStdString(ctx, 0);
    if (impl->self && impl->self->onNavigate)
        impl->self->onNavigate(impl->resolveUrl(u));
    return 0;
}

duk_ret_t loc_part_get(duk_context* ctx) {
    // Which part is requested is passed via the function's magic value.
    duk_int_t magic = duk_get_current_magic(ctx);
    JSEngine::Impl* impl = getImpl(ctx);
    Url u = parseUrl(impl->documentUrl);
    switch (magic) {
        case 0: duk_push_string(ctx, u.hostPort().c_str()); break;   // host
        case 1: duk_push_string(ctx, u.host.c_str()); break;         // hostname
        case 2: duk_push_string(ctx, u.port.c_str()); break;         // port
        case 3: duk_push_string(ctx, u.path.c_str()); break;         // pathname
        case 4: duk_push_string(ctx, u.query.empty() ? "" : ("?" + u.query).c_str()); break;
        case 5: duk_push_string(ctx, u.fragment.empty() ? "" : ("#" + u.fragment).c_str()); break;
        case 6: {                                                    // protocol
            std::string p = u.scheme.empty() ? "" : (u.scheme + ":");
            duk_push_string(ctx, p.c_str());
            break;
        }
        default: duk_push_undefined(ctx);
    }
    return 1;
}

duk_ret_t loc_assign(duk_context* ctx) {
    JSEngine::Impl* impl = getImpl(ctx);
    std::string u = dukToStdString(ctx, 0);
    if (impl->self && impl->self->onNavigate)
        impl->self->onNavigate(impl->resolveUrl(u));
    return 0;
}

duk_ret_t loc_reload(duk_context* ctx) {
    JSEngine::Impl* impl = getImpl(ctx);
    if (impl->self && impl->self->onNavigate && !impl->documentUrl.empty())
        impl->self->onNavigate(impl->documentUrl);
    return 0;
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

} // anonymous namespace

// ---------------------------------------------------------------------------
// Global API additions (round 3)
//
//   btoa / atob            — base64 for "binary strings" (byte-wise)
//   encodeURIComponent      — strict RFC 3986 percent-encoding (space is
//   decodeURIComponent        %20, NOT '+': urlEncode() is form-style)
//   requestAnimationFrame   — a 16 ms one-shot timer through the regular
//   cancelAnimationFrame      timer machinery, so the event loop's
//                             nextWakeupMs handles scheduling for free
//   navigator               — userAgent / language / platform
//   localStorage            — persistent key-value store backed by a
//   sessionStorage            small file (MB_STORAGE_FILE overrides the
//                             path; sessionStorage is memory-only, like
//                             a tab-scoped store)
// ---------------------------------------------------------------------------

static const char* kB64Chars =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string base64Encode(const std::string& in) {
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    size_t i = 0;
    while (i + 2 < in.size()) {
        unsigned v = ((unsigned char)in[i] << 16) |
                     ((unsigned char)in[i+1] << 8) |
                     (unsigned char)in[i+2];
        out += kB64Chars[(v >> 18) & 63];
        out += kB64Chars[(v >> 12) & 63];
        out += kB64Chars[(v >> 6) & 63];
        out += kB64Chars[v & 63];
        i += 3;
    }
    if (i + 1 == in.size()) {
        unsigned v = (unsigned char)in[i] << 16;
        out += kB64Chars[(v >> 18) & 63];
        out += kB64Chars[(v >> 12) & 63];
        out += "==";
    } else if (i + 2 == in.size()) {
        unsigned v = ((unsigned char)in[i] << 16) |
                     ((unsigned char)in[i+1] << 8);
        out += kB64Chars[(v >> 18) & 63];
        out += kB64Chars[(v >> 12) & 63];
        out += kB64Chars[(v >> 6) & 63];
        out += "=";
    }
    return out;
}

static int b64Val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static std::string base64Decode(const std::string& in) {
    std::string out;
    out.reserve(in.size() / 4 * 3);
    int val = 0, valb = -8;
    for (char c : in) {
        if (c == '=' || c == '\n' || c == '\r' || c == ' ' || c == '\t') {
            if (c == '=') valb = -8;
            continue;
        }
        int v = b64Val(c);
        if (v < 0) continue;
        val = (val << 6) + v;
        valb += 6;
        if (valb >= 0) {
            out += (char)((val >> valb) & 0xFF);
            valb -= 8;
        }
    }
    return out;
}

duk_ret_t js_btoa(duk_context* ctx) {
    if (duk_get_top(ctx) < 1) { duk_push_string(ctx, ""); return 1; }
    std::string s = dukToStdString(ctx, 0);
    duk_push_string(ctx, base64Encode(s).c_str());
    return 1;
}

duk_ret_t js_atob(duk_context* ctx) {
    if (duk_get_top(ctx) < 1) { duk_push_string(ctx, ""); return 1; }
    std::string s = dukToStdString(ctx, 0);
    duk_push_string(ctx, base64Decode(s).c_str());
    return 1;
}

// encodeURIComponent: escape everything except A-Za-z0-9 - _ . ! ~ * ' ( )
static std::string uriEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() + s.size() / 2);
    for (unsigned char c : s) {
        bool safe = std::isalnum(c) ||
                    c == '-' || c == '_' || c == '.' || c == '!' ||
                    c == '~' || c == '*' || c == '\'' || c == '(' ||
                    c == ')';
        if (safe) {
            out += (char)c;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

static std::string uriDecode(const std::string& s) {
    auto hexVal = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() &&
            hexVal(s[i+1]) >= 0 && hexVal(s[i+2]) >= 0) {
            out += (char)(hexVal(s[i+1]) * 16 + hexVal(s[i+2]));
            i += 2;
        } else {
            out += s[i];   // '+' stays '+' (that's urlEncode's job)
        }
    }
    return out;
}

duk_ret_t js_encodeURIComponent(duk_context* ctx) {
    if (duk_get_top(ctx) < 1) { duk_push_string(ctx, ""); return 1; }
    duk_push_string(ctx, uriEncode(dukToStdString(ctx, 0)).c_str());
    return 1;
}

duk_ret_t js_decodeURIComponent(duk_context* ctx) {
    if (duk_get_top(ctx) < 1) { duk_push_string(ctx, ""); return 1; }
    duk_push_string(ctx, uriDecode(dukToStdString(ctx, 0)).c_str());
    return 1;
}

duk_ret_t js_requestAnimationFrame(duk_context* ctx) {
    JSEngine::Impl* impl = getImpl(ctx);
    // 16 ms ~= 60 fps. Pages use rAF for paint-timed work; firing one
    // frame later keeps their sequencing (write-then-measure) intact.
    int id = makeTimer(impl, 16, false);
    JSTimer& t = impl->timers.back();
    if (duk_is_function(ctx, 0)) {
        t.isFn = true;
        t.fnStash = impl->stashFunction(0);
    } else {
        t.isFn = false;
        t.code = duk_get_top(ctx) >= 1 ? dukToStdString(ctx, 0) : "";
    }
    duk_push_int(ctx, id);
    return 1;
}

duk_ret_t js_cancelAnimationFrame(duk_context* ctx) {
    return js_clearTimer(ctx);
}

// --- localStorage / sessionStorage ---------------------------------------

struct KVStore {
    std::unordered_map<std::string, std::string> map;
    std::vector<std::string> order;   // insertion order for key(i)
    bool loaded = false;

    void set(const std::string& k, const std::string& v) {
        if (map.find(k) == map.end()) order.push_back(k);
        map[k] = v;
    }
    bool remove(const std::string& k) {
        if (map.erase(k) > 0) {
            auto it = std::find(order.begin(), order.end(), k);
            if (it != order.end()) order.erase(it);
            return true;
        }
        return false;
    }
    void clear() { map.clear(); order.clear(); }
    size_t size() const { return map.size(); }
};

// Percent-encode every byte that is not unreserved — the on-disk format
// is "key<newline>value<newline>" pairs, so ALL control characters must
// be encoded away, not just the separators.
static std::string storeEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += (char)c;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

static std::string storeDecode(const std::string& s) {
    auto hexVal = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() &&
            hexVal(s[i+1]) >= 0 && hexVal(s[i+2]) >= 0) {
            out += (char)(hexVal(s[i+1]) * 16 + hexVal(s[i+2]));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

static std::string localStoragePath() {
    if (const char* env = getenv("MB_STORAGE_FILE")) return env;
    std::string home = getenv("HOME") ? getenv("HOME") : "/tmp";
    return home + "/.minibrowser/local_storage";
}

static void localStorageLoad(KVStore& store) {
    store.loaded = true;
    FILE* f = fopen(localStoragePath().c_str(), "rb");
    if (!f) return;
    std::string content;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) content.append(buf, n);
    fclose(f);
    // Format: storeEncode(key) '\n' storeEncode(value) '\n'
    size_t p = 0;
    while (p < content.size()) {
        size_t nl1 = content.find('\n', p);
        if (nl1 == std::string::npos) break;
        size_t nl2 = content.find('\n', nl1 + 1);
        if (nl2 == std::string::npos) break;
        store.set(storeDecode(content.substr(p, nl1 - p)),
                  storeDecode(content.substr(nl1 + 1, nl2 - nl1 - 1)));
        p = nl2 + 1;
    }
}

static void localStorageSave(const KVStore& store) {
    std::string path = localStoragePath();
    // mkdir -p of the parent (best effort; single level).
    {
        std::string dir = path.substr(0, path.find_last_of('/'));
        if (!dir.empty()) mkdir(dir.c_str(), 0755);
    }
    std::string tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return;
    std::string out;
    for (const auto& k : store.order) {
        auto it = store.map.find(k);
        if (it == store.map.end()) continue;
        out += storeEncode(k) + "\n" + storeEncode(it->second) + "\n";
    }
    fwrite(out.data(), 1, out.size(), f);
    fclose(f);
    rename(tmp.c_str(), path.c_str());   // atomic swap; no torn files
}

static KVStore& sessionStore() {
    static KVStore s;   // memory-only per engine process
    s.loaded = true;
    return s;
}

static KVStore& persistentStore() {
    static KVStore s;
    if (!s.loaded) localStorageLoad(s);
    return s;
}

// The store object is created per engine at registration; the hidden
// pointer selects persistent vs session backing.
static KVStore& storeOfThis(duk_context* ctx) {
    duk_push_this(ctx);
    duk_get_prop_string(ctx, -1, "\xFF" "persistent");
    bool persistent = dukIsTruthy(ctx, -1);
    duk_pop_2(ctx);
    return persistent ? persistentStore() : sessionStore();
}

static duk_ret_t ls_getItem(duk_context* ctx) {
    if (duk_get_top(ctx) < 1) { duk_push_null(ctx); return 1; }
    auto& store = storeOfThis(ctx);
    auto it = store.map.find(dukToStdString(ctx, 0));
    if (it == store.map.end()) { duk_push_null(ctx); return 1; }
    duk_push_string(ctx, it->second.c_str());
    return 1;
}

static duk_ret_t ls_setItem(duk_context* ctx) {
    if (duk_get_top(ctx) < 2) return 0;
    auto& store = storeOfThis(ctx);
    store.set(dukToStdString(ctx, 0), dukToStdString(ctx, 1));
    if (&store == &persistentStore()) localStorageSave(store);
    return 0;
}

static duk_ret_t ls_removeItem(duk_context* ctx) {
    if (duk_get_top(ctx) < 1) return 0;
    auto& store = storeOfThis(ctx);
    store.remove(dukToStdString(ctx, 0));
    if (&store == &persistentStore()) localStorageSave(store);
    return 0;
}

static duk_ret_t ls_clear(duk_context* ctx) {
    auto& store = storeOfThis(ctx);
    store.clear();
    if (&store == &persistentStore()) localStorageSave(store);
    return 0;
}

static duk_ret_t ls_key(duk_context* ctx) {
    auto& store = storeOfThis(ctx);
    long i = (duk_get_top(ctx) >= 1 && duk_is_number(ctx, 0))
                 ? (long)duk_get_int(ctx, 0) : -1;
    if (i < 0 || i >= (long)store.order.size()) { duk_push_null(ctx); return 1; }
    duk_push_string(ctx, store.order[i].c_str());
    return 1;
}

static duk_ret_t ls_length(duk_context* ctx) {
    auto& store = storeOfThis(ctx);
    duk_push_int(ctx, (int)store.size());
    return 1;
}

static void pushStorageObject(JSEngine::Impl* impl, bool persistent) {
    duk_context* ctx = impl->ctx;
    duk_push_object(ctx);
    duk_push_boolean(ctx, persistent);
    duk_put_prop_string(ctx, -2, "\xFF" "persistent");
    duk_push_c_function(ctx, ls_getItem, 1);
    duk_put_prop_string(ctx, -2, "getItem");
    duk_push_c_function(ctx, ls_setItem, 2);
    duk_put_prop_string(ctx, -2, "setItem");
    duk_push_c_function(ctx, ls_removeItem, 1);
    duk_put_prop_string(ctx, -2, "removeItem");
    duk_push_c_function(ctx, ls_clear, 0);
    duk_put_prop_string(ctx, -2, "clear");
    duk_push_c_function(ctx, ls_key, 1);
    duk_put_prop_string(ctx, -2, "key");
    duk_push_string(ctx, "length");
    duk_push_c_function(ctx, ls_length, 0);
    duk_def_prop(ctx, -3, DUK_DEFPROP_HAVE_GETTER | DUK_DEFPROP_FORCE);
}

void jsb_register(JSEngine::Impl* impl) {
    duk_context* ctx = impl->ctx;

    // Heap stash slot 0 = the Impl pointer.
    duk_push_heap_stash(ctx);
    duk_push_pointer(ctx, (void*)impl);
    duk_put_prop_index(ctx, -2, 0);
    duk_pop(ctx);

    // Node methods prototype.
    duk_push_object(ctx);
    {
        struct M { const char* name; duk_c_function fn; int nargs; };
        static const M methods[] = {
            {"appendChild", nm_appendChild, 1},
            {"removeChild", nm_removeChild, 1},
            {"remove", nm_remove, 0},
            {"setAttribute", nm_setAttribute, 2},
            {"getAttribute", nm_getAttribute, 1},
            {"removeAttribute", nm_removeAttribute, 1},
            {"hasAttribute", nm_hasAttribute, 1},
            {"addEventListener", nm_addEventListener, 2},
            {"removeEventListener", nm_removeEventListener, 2},
            {"click", nm_click, 0},
            {"focus", nm_focus, 0},
            {"querySelector", nm_querySelector, 1},
            {"querySelectorAll", nm_querySelectorAll, 1},
            {"insertBefore", nm_insertBefore, 2},
            {"replaceChild", nm_replaceChild, 2},
            {"cloneNode", nm_cloneNode, 1},
            {"contains", nm_contains, 1},
            {"closest", nm_closest, 1},
            {"matches", nm_matches, 1},
            {"getBoundingClientRect", nm_getBoundingClientRect, 0},
            {"play", nm_media_play, 0},
            {"pause", nm_media_pause, 0},
            {nullptr, nullptr, 0}
        };
        for (int i = 0; methods[i].name; ++i) {
            duk_push_c_function(ctx, methods[i].fn, methods[i].nargs);
            duk_put_prop_string(ctx, -2, methods[i].name);
        }
    }
    // Node methods proto is exposed as a global for __makeNode.
    duk_put_global_string(ctx, "__nodeProto");

    // Trap handlers exposed as globals for the bootstrap snippet.
    duk_push_object(ctx);
    duk_push_c_function(ctx, nodeHandlerGet, 3);
    duk_put_prop_string(ctx, -2, "get");
    duk_push_c_function(ctx, nodeHandlerSet, 3);
    duk_put_prop_string(ctx, -2, "set");
    duk_put_global_string(ctx, "__nodeHandler");

    duk_push_object(ctx);
    duk_push_c_function(ctx, styleHandlerGet, 3);
    duk_put_prop_string(ctx, -2, "get");
    duk_push_c_function(ctx, styleHandlerSet, 3);
    duk_put_prop_string(ctx, -2, "set");
    duk_put_global_string(ctx, "__styleHandler");

    // document object.
    duk_push_object(ctx);
    {
        duk_push_c_function(ctx, doc_getElementById, 1);
        duk_put_prop_string(ctx, -2, "getElementById");
        duk_push_c_function(ctx, doc_querySelector, 1);
        duk_put_prop_string(ctx, -2, "querySelector");
        duk_push_c_function(ctx, doc_querySelectorAll, 1);
        duk_put_prop_string(ctx, -2, "querySelectorAll");
        duk_push_c_function(ctx, doc_createElement, 1);
        duk_put_prop_string(ctx, -2, "createElement");
        duk_push_c_function(ctx, doc_createTextNode, 1);
        duk_put_prop_string(ctx, -2, "createTextNode");
        duk_push_c_function(ctx, doc_addEventListener, 2);
        duk_put_prop_string(ctx, -2, "addEventListener");

        // Accessors: body / documentElement / title.
        // duk_def_prop pops key (-2) + value (-1); the target object sits
        // just below, at -3 (or -4 for getter+setter pairs).
        duk_push_string(ctx, "body");
        duk_push_c_function(ctx, doc_getBody, 0);
        duk_def_prop(ctx, -3, DUK_DEFPROP_HAVE_GETTER | DUK_DEFPROP_FORCE);

        duk_push_string(ctx, "documentElement");
        duk_push_c_function(ctx, doc_getDocumentElement, 0);
        duk_def_prop(ctx, -3, DUK_DEFPROP_HAVE_GETTER | DUK_DEFPROP_FORCE);

        duk_push_string(ctx, "title");
        duk_push_c_function(ctx, doc_getTitle, 0);
        duk_push_c_function(ctx, doc_setTitle, 1);
        duk_def_prop(ctx, -4, DUK_DEFPROP_HAVE_GETTER | DUK_DEFPROP_HAVE_SETTER |
                              DUK_DEFPROP_FORCE);
    }
    duk_put_global_string(ctx, "document");

    // console.
    duk_push_object(ctx);
    duk_push_c_function(ctx, console_log, DUK_VARARGS);
    duk_put_prop_string(ctx, -2, "log");
    duk_push_c_function(ctx, console_log, DUK_VARARGS);
    duk_put_prop_string(ctx, -2, "info");
    duk_push_c_function(ctx, console_log, DUK_VARARGS);
    duk_put_prop_string(ctx, -2, "warn");
    duk_push_c_function(ctx, console_err, DUK_VARARGS);
    duk_put_prop_string(ctx, -2, "error");
    duk_put_global_string(ctx, "console");

    // Globals: alert, confirm, timers, fetch, XHR.
    duk_push_c_function(ctx, js_alert, 1);
    duk_put_global_string(ctx, "alert");
    duk_push_c_function(ctx, js_confirm, 1);
    duk_put_global_string(ctx, "confirm");
    duk_push_c_function(ctx, js_setTimeout, DUK_VARARGS);
    duk_put_global_string(ctx, "setTimeout");
    duk_push_c_function(ctx, js_setInterval, DUK_VARARGS);
    duk_put_global_string(ctx, "setInterval");
    duk_push_c_function(ctx, js_clearTimer, 1);
    duk_put_global_string(ctx, "clearTimeout");
    duk_push_c_function(ctx, js_clearTimer, 1);
    duk_put_global_string(ctx, "clearInterval");
    duk_push_c_function(ctx, js_fetch, DUK_VARARGS);
    duk_put_global_string(ctx, "fetch");
    duk_push_c_function(ctx, js_XMLHttpRequest, 0);
    duk_put_global_string(ctx, "XMLHttpRequest");

    // Round-3 globals: base64, URI helpers, rAF, navigator, storage.
    duk_push_c_function(ctx, js_btoa, 1);
    duk_put_global_string(ctx, "btoa");
    duk_push_c_function(ctx, js_atob, 1);
    duk_put_global_string(ctx, "atob");
    duk_push_c_function(ctx, js_encodeURIComponent, 1);
    duk_put_global_string(ctx, "encodeURIComponent");
    duk_push_c_function(ctx, js_decodeURIComponent, 1);
    duk_put_global_string(ctx, "decodeURIComponent");
    duk_push_c_function(ctx, js_requestAnimationFrame, 1);
    duk_put_global_string(ctx, "requestAnimationFrame");
    duk_push_c_function(ctx, js_cancelAnimationFrame, 1);
    duk_put_global_string(ctx, "cancelAnimationFrame");

    duk_push_object(ctx);
    {
        // Keep the UA string in sync with net/fetch.cpp's CURLOPT_USERAGENT
        // so page JS and the network stack present the same identity.
        duk_push_string(ctx,
            "Mozilla/5.0 (X11; Linux x86_64) MiniBrowser/2.5 SDL");
        duk_put_prop_string(ctx, -2, "userAgent");
        const char* lang = getenv("MB_LANG");
        std::string l = (lang && *lang) ? lang : "en-US";
        size_t cut = l.find(',');
        if (cut != std::string::npos) l.resize(cut);
        duk_push_string(ctx, l.c_str());
        duk_put_prop_string(ctx, -2, "language");
        duk_push_string(ctx, l.c_str());
        duk_put_prop_string(ctx, -2, "languages");
        duk_push_string(ctx, "Linux x86_64");
        duk_put_prop_string(ctx, -2, "platform");
    }
    duk_put_global_string(ctx, "navigator");

    pushStorageObject(impl, /*persistent=*/true);
    duk_put_global_string(ctx, "localStorage");
    pushStorageObject(impl, /*persistent=*/false);
    duk_put_global_string(ctx, "sessionStorage");

    // location object with accessors.
    duk_push_object(ctx);
    {
        duk_push_string(ctx, "href");
        duk_push_c_function(ctx, loc_href_get, 0);
        duk_push_c_function(ctx, loc_href_set, 1);
        duk_def_prop(ctx, -4, DUK_DEFPROP_HAVE_GETTER | DUK_DEFPROP_HAVE_SETTER |
                              DUK_DEFPROP_FORCE);
        struct P { const char* name; int magic; };
        static const P parts[] = {
            {"host", 0}, {"hostname", 1}, {"port", 2}, {"pathname", 3},
            {"search", 4}, {"hash", 5}, {"protocol", 6}, {nullptr, 0}
        };
        for (int i = 0; parts[i].name; ++i) {
            duk_push_string(ctx, parts[i].name);
            duk_push_c_function(ctx, loc_part_get, 0);
            duk_set_magic(ctx, -1, parts[i].magic);
            duk_def_prop(ctx, -3, DUK_DEFPROP_HAVE_GETTER | DUK_DEFPROP_FORCE);
        }
        duk_push_c_function(ctx, loc_assign, 1);
        duk_put_prop_string(ctx, -2, "assign");
        duk_push_c_function(ctx, loc_assign, 1);
        duk_put_prop_string(ctx, -2, "replace");
        duk_push_c_function(ctx, loc_reload, 0);
        duk_put_prop_string(ctx, -2, "reload");
    }
    duk_put_global_string(ctx, "location");

    // Bootstrap: Proxy wrapper factories + window alias.
    duk_eval_string_noresult(ctx,
        "var __makeNode = function(target) {"
        "  Object.setPrototypeOf(target, __nodeProto);"
        "  return new Proxy(target, __nodeHandler); };"
        "var __makeStyle = function(target) { return new Proxy(target, __styleHandler); };"
        "var window = this;"
        "var self = this;");
}

} // namespace browser
