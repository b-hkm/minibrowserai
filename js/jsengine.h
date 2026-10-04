#pragma once
#include "../html/parser.h"
#include <functional>
#include <memory>
#include <string>

namespace browser {

// ---------------------------------------------------------------------------
// JavaScript engine, backed by an embedded Duktape (ES5.1 + extensions)
// interpreter. See js/duktape/ for the vendored engine and its license.
//
// The old home-grown line-based interpreter understood only a handful of
// statement shapes; this one runs real JavaScript: closures, functions
// with parameters and return values, loops, arrays, objects, JSON, and
// proper event callbacks.
//
// Provided browser API (matches what web pages expect):
//   document.getElementById / querySelector / querySelectorAll /
//       createElement / createTextNode / body / documentElement / title
//   node: textContent, innerHTML, id, className, classList (add/remove/
//         toggle/contains/item), value, checked, href, tagName, nodeType,
//         children, firstChild, lastChild, next/previousSibling,
//         next/previousElementSibling, parentNode, style.<prop>,
//         offsetWidth/offsetHeight/offsetTop/offsetLeft,
//         appendChild, insertBefore, replaceChild, removeChild, cloneNode,
//         contains, closest, matches, getBoundingClientRect, setAttribute,
//         getAttribute, removeAttribute, hasAttribute, remove,
//         addEventListener, removeEventListener, click, focus
//   console.log/warn/error, alert, confirm, setTimeout, setInterval,
//   clearTimeout, clearInterval, requestAnimationFrame,
//   cancelAnimationFrame, fetch (synchronous), XMLHttpRequest
//   (synchronous), location.href (navigates via onNavigate), navigator
//   (userAgent/language/platform), localStorage (file-backed),
//   sessionStorage, btoa/atob, encodeURIComponent/decodeURIComponent,
//   JSON, Math.
// ---------------------------------------------------------------------------
class JSEngine {
public:
    JSEngine();
    ~JSEngine();
    JSEngine(const JSEngine&) = delete;
    JSEngine& operator=(const JSEngine&) = delete;

    // Host callbacks -----------------------------------------------------
    std::function<void()> onDomMutated;                    // DOM changed
    std::function<void()> onInvalidate;                    // repaint wanted
    std::function<void(const std::string& url)> onNavigate; // location.href=
    std::function<void(const std::string& msg)> onAlert;    // alert(msg)
    std::function<bool(const std::string& msg)> onConfirm;  // confirm(msg)
    std::function<void(std::shared_ptr<Node> n)> onFocusNode; // el.focus()

    // Element geometry providers (round 4) — back getBoundingClientRect()
    // and the offset* properties. Wired ONCE by the host (Browser) and
    // read at call time, so they always reflect the current layout and
    // scroll position without the JS layer holding any layout pointers.
    // rectForNode: fill `out` with the node's DOCUMENT-space fragment
    //   union (layout::rectForNode). Return false when the node has no
    //   laid-out fragments — the bindings report an all-zero rect then,
    //   matching what real browsers do for display:none elements.
    // scrollY: current vertical viewport scroll offset (client coords =
    //   document coords minus this).
    // Both are inert when unset: rects simply read as zeros.
    std::function<bool(const std::shared_ptr<Node>&, DOMRect&)> rectForNode;
    std::function<int()> scrollY;

    // Media support (round 9): resolve a (possibly relative) src attribute
    // against the current document exactly the way layout resolves media
    // sources — so page JS and the layout/renderer agree on the player
    // registry key. Inert when unset (relative srcs stay relative).
    std::function<std::string(const std::string&)> resolveMedia;

    // Execution ----------------------------------------------------------
    bool execute(const std::string& code);   // false on parse/exec error

    // Run `code` as an event handler with a real `event` object
    // ({ type, target }) in scope.
    bool executeEvent(const std::string& code, const std::string& type,
                      std::shared_ptr<Node> target);

    // Fire listeners registered via addEventListener on `target`, then
    // bubble up through ancestors and finally document-level listeners.
    // Returns true when at least one handler ran.
    bool dispatchEvent(const std::string& type, std::shared_ptr<Node> target);

    // Call a top-level function by name.
    bool callFunction(const std::string& name);

    // Fire due timers. Returns true if any fired.
    bool pumpTimers();

    // Milliseconds until the soonest pending timer fires, or -1 when no
    // timer is scheduled. Lets the event loop sleep between wakeups.
    int nextTimerDelayMs() const;

    // Document -----------------------------------------------------------
    void setDocument(std::shared_ptr<Node> doc);
    std::shared_ptr<Node> getDocument() const;
    // URL of the current page (for location.* and relative fetch()).
    void setDocumentUrl(const std::string& url);

    // Test/inspection helper: evaluate an expression, return its
    // string form (like console.log would print).
    std::string evaluateToString(const std::string& expr);

    // Opaque engine state; defined in js/js_internal.h, shared with
    // jsbindings.cpp.
    struct Impl;

private:
    Impl* impl;
};

} // namespace browser
