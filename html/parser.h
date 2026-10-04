#pragma once
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace browser {

struct Node {
    std::string tag;
    std::string text;
    std::map<std::string, std::string> attrs;
    std::vector<std::shared_ptr<Node>> children;
    std::weak_ptr<Node> parent;
};

// A rectangle in CSS pixels, field-named after the DOM's DOMRect so the
// JS bindings can expose it directly. Layout code produces DOCUMENT-space
// rects; the JS bindings subtract the scroll offset to hand page scripts
// the viewport-relative numbers the real DOMRect spec calls for.
struct DOMRect {
    float x = 0, y = 0;
    float width = 0, height = 0;
    float left()   const { return x; }
    float top()    const { return y; }
    float right()  const { return x + width; }
    float bottom() const { return y + height; }
};

std::shared_ptr<Node> parseHTML(const std::string& html);
std::string extractJS(const std::shared_ptr<Node>& root);

// One <script> of the document, in document order. src non-empty =
// external script (fetch it); otherwise body holds the inline code.
struct JSScriptChunk {
    std::string src;
    std::string body;
};
// Document-order script chunks (inline bodies + external src markers so
// the caller can prefetch in parallel and run with a time budget).
void extractJSChunks(const std::shared_ptr<Node>& root,
                     std::vector<JSScriptChunk>& out);

std::string extractCSS(const std::shared_ptr<Node>& root);
// Find the first node with the given id attribute. Returns nullptr if not found.
std::shared_ptr<Node> findById(const std::shared_ptr<Node>& root, const std::string& id);
// Value of the first <base href="..."> in the document (document order),
// or "" when the page declares none. Callers resolve it against the real
// page URL — a <base> may itself be relative.
std::string findBaseHref(const std::shared_ptr<Node>& root);
// Walk up from `start` looking for an ancestor (or self) that has the
// given attribute set. Returns the attribute value, or "" if not found.
std::string findAttr(const std::shared_ptr<Node>& start,
                     const std::string& attr);

} // namespace browser
