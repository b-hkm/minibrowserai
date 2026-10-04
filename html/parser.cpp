#include "parser.h"
#include "lexer.h"
#include <functional>
#include <unordered_set>

namespace browser {

static const std::unordered_set<std::string> kVoidTags = {
    "br","img","hr","input","meta","link","area","base","col",
    "embed","param","source","track","wbr"
};

// ---------------------------------------------------------------------------
// Implied end tags ("optional" tags in the HTML spec). Real-world HTML —
// hand-written pages, WordPress themes, Wikipedia lists, forums — relies
// on the parser closing <li>/<p>/<td>/... automatically. Without this,
// `<li>a<li>b` nested b inside a and the whole list rendered as one item.
//
// The rules below are a pragmatic subset of the HTML5 tree-construction
// "close a <p> element" / list-item-scope / table-scope actions:
//   * new block element  -> closes an open <p> (unless a scope boundary
//                           sits between them: table cells, buttons...)
//   * new <li>           -> closes an open <li> before any <ul>/<ol>
//   * new <dt>/<dd>      -> closes open <dt>/<dd> before <dl>
//   * new <tr> / section -> closes open <td>/<th>/<tr> (+ section) before <table>
//   * new <td>/<th>      -> closes open <td>/<th> before <tr>/<table>
//   * new <option>       -> closes an open <option> (or <optgroup> for
//                           a new <optgroup>) before <select>
// ---------------------------------------------------------------------------
static bool isBlockThatClosesP(const std::string& t) {
    static const std::unordered_set<std::string> blocks = {
        "address","article","aside","blockquote","details","dialog","dir",
        "div","dl","fieldset","figcaption","figure","footer","form","h1",
        "h2","h3","h4","h5","h6","header","hgroup","hr","main","menu","nav",
        "ol","p","pre","section","table","ul","li","dt","dd","center"
    };
    return blocks.count(t) > 0;
}

// Elements that stop the upward search for an open tag to imply-close.
// Mirrors the HTML5 "button scope" boundaries (subset): once the walk
// crosses one of these, whatever is above is somebody else's subtree.
static const std::unordered_set<std::string> kScopeBounds = {
    "html","body","table","caption","td","th","button","object",
    "template","marquee","applet"
};

// Close elements on the stack. Walks up to the first tag in `targets`
// and pops it (with everything above), then keeps popping while the new
// top is ALSO a target — an implied `</td>` transitively closes its
// `</tr>`. Never crosses a `boundaries` element.
static void closeUpTo(std::vector<std::shared_ptr<Node>>& stack,
                      const std::unordered_set<std::string>& targets,
                      const std::unordered_set<std::string>& boundaries,
                      const std::string& /*newTag*/) {
    for (size_t k = stack.size(); k > 1; --k) {
        const std::string& t = stack[k - 1]->tag;
        if (targets.count(t)) {
            stack.resize(k - 1);   // pop the matched element and above
            // Stacked targets close together: </td> implies </tr>.
            while (stack.size() > 1 &&
                   targets.count(stack.back()->tag)) {
                stack.pop_back();
            }
            return;
        }
        if (boundaries.count(t)) return;   // scope ends here — nothing closed
    }
}

static void applyImpliedEndTags(const std::string& newTag,
                                std::vector<std::shared_ptr<Node>>& stack) {
    static const std::unordered_set<std::string> pSet = {"p"};
    static const std::unordered_set<std::string> liSet = {"li"};
    static const std::unordered_set<std::string> liBounds = {
        "html","body","table","td","th","caption","button","object",
        "template","marquee","applet","ul","ol"
    };
    static const std::unordered_set<std::string> dtddSet = {"dt","dd"};
    static const std::unordered_set<std::string> dtddBounds = {
        "html","body","table","td","th","caption","button","object",
        "template","marquee","applet","dl"
    };
    static const std::unordered_set<std::string> trSet = {"tr","td","th"};
    static const std::unordered_set<std::string> sectionSet = {
        "thead","tbody","tfoot","tr","td","th"
    };
    static const std::unordered_set<std::string> tdSet = {"td","th"};
    static const std::unordered_set<std::string> tableBounds = {
        "html","body","table","caption"
    };
    static const std::unordered_set<std::string> optSet = {"option"};
    static const std::unordered_set<std::string> optgrpSet = {"option","optgroup"};
    static const std::unordered_set<std::string> selectBounds = {
        "html","body","select"
    };

    if (isBlockThatClosesP(newTag)) {
        // <p> in button scope: walk up; inline elements (b/i/span/em/)
        // AND containers (ul/ol/li/div...) do not block the search —
        // HTML5 closes the p through them — but table cells / buttons
        // do. `<li><p>a<li>` must close BOTH the p and the li.
        closeUpTo(stack, pSet, kScopeBounds, newTag);
    }
    if (newTag == "li")            closeUpTo(stack, liSet, liBounds, newTag);
    else if (newTag == "dt" || newTag == "dd")
                                   closeUpTo(stack, dtddSet, dtddBounds, newTag);
    else if (newTag == "tr")       closeUpTo(stack, trSet, tableBounds, newTag);
    else if (newTag == "thead" || newTag == "tbody" || newTag == "tfoot")
                                   closeUpTo(stack, sectionSet, tableBounds, newTag);
    else if (newTag == "td" || newTag == "th")
                                   closeUpTo(stack, tdSet, tableBounds, newTag);
    else if (newTag == "option")   closeUpTo(stack, optSet, selectBounds, newTag);
    else if (newTag == "optgroup") closeUpTo(stack, optgrpSet, selectBounds, newTag);
}

std::shared_ptr<Node> parseHTML(const std::string& html) {
    auto root = std::make_shared<Node>();
    root->tag = "root";
    std::vector<std::shared_ptr<Node>> stack = {root};

    auto toks = tokenize(html);
    std::vector<std::shared_ptr<Node>> pendingText;

    auto flushText = [&]() {
        std::string joined;
        for (auto& n : pendingText) joined += n->text;
        if (!joined.empty()) {
            auto t = std::make_shared<Node>();
            t->tag = "text";
            t->text = joined;
            t->parent = stack.back();
            stack.back()->children.push_back(t);
        }
        pendingText.clear();
    };

    for (auto& tk : toks) {
        switch (tk.kind) {
            case TokKind::TEXT: {
                auto t = std::make_shared<Node>();
                t->tag = "text";
                t->text = tk.text;
                pendingText.push_back(t);
                break;
            }
            case TokKind::COMMENT:
            case TokKind::DOCTYPE:
                flushText();
                break;
            case TokKind::OPEN: {
                flushText();
                // HTML5 implied end tags: `<li>a<li>b`, `<p>one<p>two`,
                // `<tr><td>x<tr>` all close their open predecessors here.
                applyImpliedEndTags(tk.name, stack);
                auto n = std::make_shared<Node>();
                n->tag = tk.name;
                n->attrs = tk.attrs;
                n->parent = stack.back();
                stack.back()->children.push_back(n);
                if (!kVoidTags.count(tk.name)) stack.push_back(n);
                break;
            }
            case TokKind::SELFCLOSE: {
                flushText();
                auto n = std::make_shared<Node>();
                n->tag = tk.name;
                n->attrs = tk.attrs;
                n->parent = stack.back();
                stack.back()->children.push_back(n);
                break;
            }
            case TokKind::CLOSE: {
                flushText();
                // Find the nearest OPEN ancestor on the stack whose tag
                // matches the close tag. If found, pop everything down to
                // (and including) that ancestor. If NOT found (stray
                // </div> with no matching <div>), do nothing — the old
                // code popped ALL the way to the root, which corrupts the
                // tree for the rest of the document.
                size_t match = std::string::npos;
                for (size_t k = stack.size(); k > 1; --k) {
                    if (stack[k - 1]->tag == tk.name) {
                        match = k - 1;
                        break;
                    }
                }
                if (match == std::string::npos) {
                    // Stray close tag — ignore it.
                    break;
                }
                stack.resize(match);
                break;
            }
            case TokKind::EOF_:
                flushText();
                break;
        }
    }

    // This engine always ships a JavaScript runtime (duktape), so pages
    // styled for JS-capable browsers must see the marker class MediaWiki
    // & co. add from JS (documentElement.className += ' client-js').
    // Without it, progressive-enhancement gates hide content behind
    // `.client-js ...` rules and dropdown menus render unstyled.
    for (auto& c : root->children) {
        if (c->tag == "html") {
            auto it = c->attrs.find("class");
            std::string cls = (it == c->attrs.end()) ? "" : it->second;
            if (cls.find("client-js") == std::string::npos) {
                cls += (cls.empty() ? "" : " ");
                cls += "client-js";
                c->attrs["class"] = cls;
            }
            break;
        }
    }
    return root;
}

std::string extractJS(const std::shared_ptr<Node>& root) {
    std::string js;
    std::function<void(const std::shared_ptr<Node>&)> walk =
        [&](const std::shared_ptr<Node>& n) {
            for (auto& c : n->children) {
                if (c->tag == "script") {
                    std::string body;
                    for (auto& sc : c->children)
                        if (sc->tag == "text") body += sc->text;
                    if (!body.empty()) js += body + "\n";
                } else {
                    walk(c);
                }
            }
        };
    walk(root);
    return js;
}

void extractJSChunks(const std::shared_ptr<Node>& root,
                     std::vector<JSScriptChunk>& out) {
    std::function<void(const std::shared_ptr<Node>&)> walk =
        [&](const std::shared_ptr<Node>& n) {
            for (auto& c : n->children) {
                if (c->tag == "script") {
                    std::string body;
                    for (auto& sc : c->children)
                        if (sc->tag == "text") body += sc->text;
                    if (!body.empty())
                        out.push_back(JSScriptChunk{"", body});
                } else {
                    walk(c);
                }
            }
        };
    walk(root);
}

std::string extractCSS(const std::shared_ptr<Node>& root) {
    std::string css;
    std::function<void(const std::shared_ptr<Node>&)> walk =
        [&](const std::shared_ptr<Node>& n) {
            for (auto& c : n->children) {
                if (c->tag == "style") {
                    std::string body;
                    for (auto& sc : c->children)
                        if (sc->tag == "text") body += sc->text;
                    if (!body.empty()) css += body + "\n";
                } else {
                    walk(c);
                }
            }
        };
    walk(root);
    return css;
}

std::shared_ptr<Node> findById(const std::shared_ptr<Node>& root, const std::string& id) {
    if (!root) return nullptr;
    auto it = root->attrs.find("id");
    if (it != root->attrs.end() && it->second == id) return root;
    for (auto& c : root->children) {
        auto found = findById(c, id);
        if (found) return found;
    }
    return nullptr;
}

std::string findBaseHref(const std::shared_ptr<Node>& root) {
    if (!root) return "";
    // Document order walk; the FIRST <base href> wins (HTML: subsequent
    // <base> elements are ignored).
    std::function<std::string(const std::shared_ptr<Node>&)> walk =
        [&](const std::shared_ptr<Node>& n) -> std::string {
            for (auto& c : n->children) {
                if (c->tag == "base") {
                    auto it = c->attrs.find("href");
                    if (it != c->attrs.end() && !it->second.empty())
                        return it->second;
                }
                if (c->tag == "body") return "";  // <base> lives in <head>
                if (auto r = walk(c); !r.empty()) return r;
            }
            return "";
        };
    return walk(root);
}

std::string findAttr(const std::shared_ptr<Node>& start,
                     const std::string& attr) {
    for (auto n = start; n; n = n->parent.lock()) {
        auto it = n->attrs.find(attr);
        if (it != n->attrs.end()) return it->second;
    }
    return "";
}

} // namespace browser

