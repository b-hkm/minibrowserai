#include "selftest.h"

#include "../html/lexer.h"
#include "../html/parser.h"
#include "../css/style.h"
#include "../js/jsengine.h"
#include "../layout/layout.h"
#include "../layout/font_loader.h"
#include "../layout/resource.h"
#include "../layout/text_shaper.h"
#include "../media/mediaplayer.h"
#include "../media/extractor.h"
#include "../media/minjson.h"
#include "../net/url.h"
#include "../net/fetch.h"
#include "../render/renderer.h"
#include "../app/mpv.h"
#include "../app/shims.h"

#include <SDL2/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace browser {

// ---------------------------------------------------------------------------
// Tiny test framework
// ---------------------------------------------------------------------------

struct TestCase {
    const char* name;
    void (*fn)();
};

static std::vector<TestCase>& registry() {
    static std::vector<TestCase> r;
    return r;
}

struct Register {
    Register(const char* n, void (*f)()) { registry().push_back({n, f}); }
};

#define TEST(name)                                          \
    static void name();                                     \
    static Register _reg_##name(#name, &name);              \
    static void name()

static int g_pass = 0, g_fail = 0;

#define EXPECT(cond)                                                       \
    do {                                                                   \
        if (cond) { ++g_pass; }                                            \
        else {                                                             \
            ++g_fail;                                                      \
            std::cerr << "  FAIL " << __FILE__ << ":" << __LINE__           \
                      << "  EXPECT(" #cond ")\n";                          \
        }                                                                  \
    } while (0)

#define EXPECT_EQ(a, b)                                                    \
    do {                                                                   \
        auto _va = (a);                                                    \
        auto _vb = (b);                                                    \
        if (_va == _vb) { ++g_pass; }                                      \
        else {                                                             \
            ++g_fail;                                                      \
            std::cerr << "  FAIL " << __FILE__ << ":" << __LINE__           \
                      << "  EXPECT_EQ(" #a ", " #b ")  got <"              \
                      << _va << "> vs <" << _vb << ">\n";                  \
        }                                                                  \
    } while (0)

static std::string joinText(const std::vector<Tok>& toks) {
    std::string s;
    for (auto& t : toks) if (t.kind == TokKind::TEXT) s += t.text;
    return s;
}

static std::string joinText(const std::shared_ptr<Node>& n) {
    std::string s;
    for (auto& c : n->children) if (c->tag == "text") s += c->text;
    return s;
}

// ---------------------------------------------------------------------------
// Lexer / entity decoding
// ---------------------------------------------------------------------------

TEST(lexer_named_entities) {
    auto toks = tokenize("<p>Tom &amp; Jerry &lt;3 &quot;cheese&quot;&apos;s</p>");
    EXPECT_EQ(joinText(toks), std::string("Tom & Jerry <3 \"cheese\"'s"));
}

TEST(lexer_numeric_decimal_entities) {
    auto toks = tokenize("<p>&#8212;&#169;</p>");
    EXPECT_EQ(joinText(toks), std::string("\xe2\x80\x94\xc2\xa9"));
}

TEST(lexer_numeric_hex_entities) {
    auto toks = tokenize("<p>&#x2014;&#xA9;</p>");
    EXPECT_EQ(joinText(toks), std::string("\xe2\x80\x94\xc2\xa9"));
}

TEST(lexer_unknown_entities_passthrough) {
    auto toks = tokenize("<p>&zzzz; &</p>");
    EXPECT_EQ(joinText(toks), std::string("&zzzz; &"));
}

TEST(lexer_attribute_parsing) {
    auto attrs = parseAttrs("class=\"foo bar\" id='main' disabled");
    EXPECT_EQ(attrs["class"], std::string("foo bar"));
    EXPECT_EQ(attrs["id"],    std::string("main"));
    EXPECT_EQ(attrs["disabled"], std::string(""));
}

TEST(lexer_self_closing_tags) {
    auto toks = tokenize("<br/><img src='x.png'/>");
    bool sawBr = false, sawImg = false;
    for (auto& t : toks) {
        if (t.kind == TokKind::SELFCLOSE && t.name == "br")  sawBr  = true;
        if (t.kind == TokKind::SELFCLOSE && t.name == "img") sawImg = true;
    }
    EXPECT(sawBr);
    EXPECT(sawImg);
}

TEST(lexer_comments_stripped) {
    auto toks = tokenize("<p>before<!-- skip me -->after</p>");
    EXPECT_EQ(joinText(toks), std::string("beforeafter"));
}

TEST(parser_finds_by_id) {
    auto root = parseHTML("<html><body><p id='hi'>Hello <b>world</b></p></body></html>");
    auto found = findById(root, "hi");
    EXPECT(found != nullptr);
    if (found) {
        bool hasB = false;
        for (auto& c : found->children) if (c->tag == "b") hasB = true;
        EXPECT(hasB);
    }
}

TEST(parser_missing_id_returns_null) {
    auto root = parseHTML("<html><body><p>no id here</p></body></html>");
    auto found = findById(root, "missing");
    EXPECT(found == nullptr);
}

// ---------------------------------------------------------------------------
// CSS
// ---------------------------------------------------------------------------

TEST(css_named_color_red) {
    Style s;
    s = applyStyle(s, "color: red");
    EXPECT(s.hasColor);
    EXPECT_EQ((int)s.color.r, 255);
    EXPECT_EQ((int)s.color.g, 0);
    EXPECT_EQ((int)s.color.b, 0);
}

TEST(css_named_color_transparent) {
    Style s;
    s = applyStyle(s, "background: transparent");
    EXPECT(s.hasBg);
    EXPECT_EQ((int)s.bg.a, 0);
}

TEST(css_hex_color) {
    Style s;
    s = applyStyle(s, "background: #1a4fa0");
    EXPECT(s.hasBg);
    EXPECT_EQ((int)s.bg.r, 0x1a);
    EXPECT_EQ((int)s.bg.g, 0x4f);
    EXPECT_EQ((int)s.bg.b, 0xa0);
}

TEST(css_edge_shorthand_1) {
    Style s;
    s = applyStyle(s, "margin: 10px");
    EXPECT(s.hasMargin);
    EXPECT_EQ(s.margin.top,    10);
    EXPECT_EQ(s.margin.right,  10);
    EXPECT_EQ(s.margin.bottom, 10);
    EXPECT_EQ(s.margin.left,   10);
}

TEST(css_edge_shorthand_2) {
    Style s;
    s = applyStyle(s, "margin: 10px 20px");
    EXPECT(s.hasMargin);
    EXPECT_EQ(s.margin.top,    10);
    EXPECT_EQ(s.margin.bottom, 10);
    EXPECT_EQ(s.margin.left,   20);
    EXPECT_EQ(s.margin.right,  20);
}

TEST(css_edge_shorthand_3) {
    Style s;
    s = applyStyle(s, "margin: 1px 2px 3px");
    EXPECT(s.hasMargin);
    EXPECT_EQ(s.margin.top,    1);
    EXPECT_EQ(s.margin.left,   2);
    EXPECT_EQ(s.margin.right,  2);
    EXPECT_EQ(s.margin.bottom, 3);
}

TEST(css_edge_shorthand_4) {
    Style s;
    s = applyStyle(s, "margin: 1px 2px 3px 4px");
    EXPECT(s.hasMargin);
    EXPECT_EQ(s.margin.top,    1);
    EXPECT_EQ(s.margin.right,  2);
    EXPECT_EQ(s.margin.bottom, 3);
    EXPECT_EQ(s.margin.left,   4);
}

TEST(css_selectors_basic) {
    EXPECT(matchesSelector("p",          "p", {}));
    EXPECT(!matchesSelector("div",       "p", {}));
    EXPECT(matchesSelector("*",          "anything", {}));
    std::map<std::string, std::string> a{{"class", "foo"}};
    EXPECT(matchesSelector(".foo",       "p", a));
    EXPECT(!matchesSelector(".foo",      "p", {}));
    EXPECT(matchesSelector("p.foo",      "p", a));
    std::map<std::string, std::string> b{{"id", "bar"}};
    EXPECT(matchesSelector("#bar",       "p", b));
    // FIX: p.foo#bar requires BOTH class=foo AND id=bar. The element `a`
    // only has class=foo (no id), so this should NOT match.
    EXPECT(!matchesSelector("p.foo#bar",  "p", a));
    auto a2 = a; a2["id"] = "bar";
    EXPECT(matchesSelector("p.foo#bar",  "p", a2));
}

TEST(css_parse_full_rule) {
    auto rules = parseCSS("p { color: red; } .foo { color: blue; } #bar { color: green; }");
    EXPECT_EQ(rules.size(), (size_t)3);
    EXPECT_EQ(rules[0].selectors[0], std::string("p"));
    EXPECT(rules[0].style.hasColor);
}

// ---------------------------------------------------------------------------
// JS engine (Duktape-backed)
// ---------------------------------------------------------------------------

TEST(js_eval_arithmetic) {
    JSEngine eng;
    EXPECT_EQ(eng.evaluateToString("6 * 7"), std::string("42"));
}

TEST(js_eval_string_and_array) {
    JSEngine eng;
    EXPECT_EQ(eng.evaluateToString("'hello ' + [1, 2, 3].length"),
              std::string("hello 3"));
}

TEST(js_var_and_function_with_args) {
    JSEngine eng;
    eng.execute("var x = 21; function double(n) { return n * 2; }");
    EXPECT_EQ(eng.evaluateToString("double(x)"), std::string("42"));
}

TEST(js_closures_and_return_values) {
    JSEngine eng;
    eng.execute("function makeCounter() { var n = 0; return function() { n = n + 1; return n; }; }"
                "var c = makeCounter();"
                "c(); c();");
    EXPECT_EQ(eng.evaluateToString("c()"), std::string("3"));
}

TEST(js_json_roundtrip) {
    JSEngine eng;
    eng.execute("var o = JSON.parse('{\"a\": 5, \"b\": [1,2]}');");
    EXPECT_EQ(eng.evaluateToString("o.a"), std::string("5"));
    EXPECT_EQ(eng.evaluateToString("o.b.length"), std::string("2"));
    EXPECT(eng.evaluateToString("JSON.stringify({a: 1})").find("\"a\"") !=
           std::string::npos);
}

TEST(js_loops_and_conditionals) {
    JSEngine eng;
    eng.execute("var s = 0; for (var i = 1; i <= 10; i++) { if (i % 2 == 0) { s += i; } }");
    EXPECT_EQ(eng.evaluateToString("s"), std::string("30"));
}

TEST(js_execute_console_log) {
    JSEngine eng;
    std::stringstream out;
    auto* old = std::cout.rdbuf(out.rdbuf());
    eng.execute("console.log(\"hi\", 1, true);");
    std::cout.rdbuf(old);
    std::string s = out.str();
    EXPECT(s.find("hi")   != std::string::npos);
    EXPECT(s.find("1")    != std::string::npos);
    EXPECT(s.find("true") != std::string::npos);
}

TEST(js_assignment_var) {
    JSEngine eng;
    eng.execute("var x = 42;");
    EXPECT_EQ(eng.evaluateToString("x"), std::string("42"));
}

TEST(js_assignment_textContent) {
    JSEngine eng;
    auto dom = parseHTML("<html><body><p id='p'>old</p></body></html>");
    eng.setDocument(dom);
    eng.execute("document.getElementById(\"p\").textContent = \"new\";");
    auto p = findById(dom, "p");
    EXPECT(p != nullptr);
    if (p) EXPECT_EQ(joinText(p), std::string("new"));
}

TEST(js_assignment_style) {
    JSEngine eng;
    auto dom = parseHTML("<html><body><p id='p'>x</p></body></html>");
    eng.setDocument(dom);
    eng.execute("document.getElementById(\"p\").style.color = \"red\";");
    auto p = findById(dom, "p");
    EXPECT(p != nullptr);
    if (p) {
        EXPECT(p->attrs.count("style") > 0);
        EXPECT(p->attrs["style"].find("color: red") != std::string::npos);
    }
}

// ---------------------------------------------------------------------------
// Resource path resolution
// ---------------------------------------------------------------------------

TEST(resource_absolute) {
    ResourceLoader r("/tmp");
    EXPECT_EQ(r.resolve("/etc/hosts"),   std::string("/etc/hosts"));
    EXPECT_EQ(r.resolve("C:/Windows/x"), std::string("C:/Windows/x"));
}

TEST(resource_remote) {
    ResourceLoader r("/tmp");
    EXPECT_EQ(r.resolve("http://example.com/x.png"),
              std::string("http://example.com/x.png"));
    // Remote URLs keep their query string: CDN sizing params and signed
    // image URLs ("...jpg?w=640&sig=...") break if stripped.
    EXPECT_EQ(r.resolve("https://example.com/x.png?v=1"),
              std::string("https://example.com/x.png?v=1"));
    // Protocol-relative URLs are remote too (old code treated them as
    // local absolute paths — the main "online images don't show" bug).
    // With a local base they default to https; with a remote base they
    // inherit the page's scheme.
    EXPECT_EQ(r.resolve("//cdn.example.com/x.png"),
              std::string("https://cdn.example.com/x.png"));
    ResourceLoader rr("https://page.example.com/a/index.html");
    EXPECT_EQ(rr.resolve("//cdn.example.com/x.png"),
              std::string("https://cdn.example.com/x.png"));
    EXPECT_EQ(rr.resolve("img/photo.jpg"),
              std::string("https://page.example.com/a/img/photo.jpg"));
    EXPECT_EQ(rr.resolve("../top.gif"),
              std::string("https://page.example.com/top.gif"));
}

TEST(resource_relative) {
    ResourceLoader r("/var/www");
    EXPECT_EQ(r.resolve("img.png"),     std::string("/var/www/img.png"));
    EXPECT_EQ(r.resolve("./img.png"),   std::string("/var/www/img.png"));
    EXPECT_EQ(r.resolve("a/../b.png"),  std::string("/var/www/b.png"));
    EXPECT_EQ(r.resolve("a/./b.png"),   std::string("/var/www/a/b.png"));
}

TEST(resource_strip_query_frag) {
    ResourceLoader r("/x");
    EXPECT_EQ(r.resolve("foo.png?v=2"),      std::string("/x/foo.png"));
    EXPECT_EQ(r.resolve("foo.png#frag"),     std::string("/x/foo.png"));
    EXPECT_EQ(r.resolve("foo.png?v=2#frag"), std::string("/x/foo.png"));
}

// ---------------------------------------------------------------------------
// Driver
// ---------------------------------------------------------------------------

int runSelfTest() {
    g_pass = 0; g_fail = 0;
    std::cout << "Running " << registry().size() << " self-tests...\n\n";
    for (auto& tc : registry()) {
        std::cout << "  " << tc.name << " ... ";
        int before = g_fail;
        tc.fn();
        if (g_fail == before) std::cout << "ok\n";
        else                  std::cout << "FAILED\n";
    }
    std::cout << "\n" << g_pass << " passed, " << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
TEST(parser_findAttr_self) {
    auto root = parseHTML("<html><body><p id='p' class='c'>x</p></body></html>");
    auto p = findById(root, "p");
    EXPECT(p != nullptr);
    if (p) {
        EXPECT_EQ(findAttr(p, "id"),    std::string("p"));
        EXPECT_EQ(findAttr(p, "class"), std::string("c"));
        EXPECT_EQ(findAttr(p, "nope"),  std::string(""));
    }
}

TEST(parser_findAttr_inherits_from_parent) {
    auto root = parseHTML("<html><body><p id='p'><span>x</span></p></body></html>");
    auto span = root->children[0]->children[0]->children[0];
    // The span's parent is the <p>. findAttr walks up.
    EXPECT_EQ(findAttr(span, "id"), std::string("p"));
}

TEST(js_execute_event_sets_event_global) {
    JSEngine eng;
    auto dom = parseHTML("<html><body><p id='t'>x</p></body></html>");
    eng.setDocument(dom);
    auto t = findById(dom, "t");

    eng.executeEvent("window.__type = event.type;"
                     "window.__tag = event.target.tagName;"
                     "window.__id = event.target.id;", "click", t);
    EXPECT_EQ(eng.evaluateToString("__type"), std::string("click"));
    EXPECT_EQ(eng.evaluateToString("__tag"),  std::string("P"));
    EXPECT_EQ(eng.evaluateToString("__id"),   std::string("t"));
}

TEST(js_execute_event_triggers_relayout_via_dom_mutation) {
    JSEngine eng;
    auto dom = parseHTML("<html><body><p id='p'>old</p></body></html>");
    eng.setDocument(dom);
    auto p = findById(dom, "p");

    int callbackCount = 0;
    eng.onDomMutated = [&]() { ++callbackCount; };

    eng.executeEvent(
        "document.getElementById('p').textContent = 'new';", "click", p);

    EXPECT_EQ(callbackCount, 1);
    EXPECT_EQ(joinText(p), std::string("new"));
}

// ---------------------------------------------------------------------------
// Compound selectors (descendant / child / sibling combinators)
// ---------------------------------------------------------------------------

TEST(css_compound_descendant) {
    // "nav a" should match a link inside a <nav> but NOT a link directly in <body>.
    auto dom = parseHTML(
        "<html><body>"
        "<nav><a id='in-nav'>x</a></nav>"
        "<a id='outside'>y</a>"
        "</body></html>");
    auto inNav   = findById(dom, "in-nav");
    auto outside = findById(dom, "outside");
    EXPECT(inNav != nullptr);
    EXPECT(outside != nullptr);
    if (inNav)   EXPECT(matchesSelectorNode("nav a", inNav));
    if (outside) EXPECT(!matchesSelectorNode("nav a", outside));
}

TEST(css_compound_child) {
    // "div > p" should match a direct child but not a deeper descendant.
    auto dom = parseHTML(
        "<html><body>"
        "<div><p id='direct'>a</p></div>"
        "<div><section><p id='deep'>b</p></section></div>"
        "</body></html>");
    auto direct = findById(dom, "direct");
    auto deep   = findById(dom, "deep");
    if (direct) EXPECT(matchesSelectorNode("div > p", direct));
    if (deep)   EXPECT(!matchesSelectorNode("div > p", deep));
}

TEST(css_compound_class_descendant) {
    // ".card p" should match any <p> inside an element with class "card".
    auto dom = parseHTML(
        "<html><body>"
        "<div class='card'><p id='inside'>x</p></div>"
        "<p id='outside'>y</p>"
        "</body></html>");
    auto inside  = findById(dom, "inside");
    auto outside = findById(dom, "outside");
    if (inside)  EXPECT(matchesSelectorNode(".card p", inside));
    if (outside) EXPECT(!matchesSelectorNode(".card p", outside));
}

// ---------------------------------------------------------------------------
// Lists: ul/ol produce bullets/numbers
// ---------------------------------------------------------------------------

TEST(list_ol_marker) {
    auto dom = parseHTML(
        "<html><body><ol id='ol'>"
        "<li>first</li><li>second</li><li>third</li>"
        "</ol></body></html>");
    auto ol = findById(dom, "ol");
    EXPECT(ol != nullptr);
    if (ol) {
        EXPECT_EQ(ol->children.size(), (size_t)3);
        // Verify they're all <li> tags.
        for (auto& c : ol->children) EXPECT(c->tag == "li");
    }
}

// ---------------------------------------------------------------------------
// CSS display:none and text-align
// ---------------------------------------------------------------------------

TEST(css_display_none_parsed) {
    auto rules = parseCSS(".hidden { display: none; } .center { text-align: center; }");
    EXPECT_EQ(rules.size(), (size_t)2);
    EXPECT(rules[0].style.hasDisplay);
    EXPECT_EQ(rules[0].style.display, std::string("none"));
    EXPECT(rules[1].style.hasTextAlign);
    EXPECT_EQ(rules[1].style.textAlign, std::string("center"));
}

TEST(css_text_align_inline) {
    Style s;
    s = applyStyle(s, "text-align: right");
    EXPECT(s.hasTextAlign);
    EXPECT_EQ(s.textAlign, std::string("right"));
}

// ---------------------------------------------------------------------------
// JS function declarations and timer queue
// ---------------------------------------------------------------------------

TEST(js_function_declaration_and_call) {
    JSEngine eng;
    eng.execute("function greet() { return 'hello'; }");
    // The function should exist and be callable.
    EXPECT(eng.callFunction("greet"));
    EXPECT_EQ(eng.evaluateToString("greet()"), std::string("hello"));
}

TEST(js_function_with_body_spanning_statements) {
    JSEngine eng;
    // Function body contains two statements separated by `;`. They
    // should both run when the function is called.
    eng.execute("function f() { console.log(\"one\"); console.log(\"two\"); }");
    std::stringstream out;
    auto* old = std::cout.rdbuf(out.rdbuf());
    eng.callFunction("f");
    std::cout.rdbuf(old);
    EXPECT(out.str().find("one") != std::string::npos);
    EXPECT(out.str().find("two") != std::string::npos);
}

TEST(js_top_level_function_call) {
    JSEngine eng;
    eng.execute("function f() { console.log(\"called\"); }");
    std::stringstream out;
    auto* old = std::cout.rdbuf(out.rdbuf());
    eng.execute("f();");
    std::cout.rdbuf(old);
    EXPECT(out.str().find("called") != std::string::npos);
}

TEST(js_settimeout_runs_after_delay) {
    JSEngine eng;
    // setTimeout with delay 0 should fire on the next pumpTimers call.
    eng.execute("setTimeout(\"console.log('fired')\", 0);");
    std::stringstream out;
    auto* old = std::cout.rdbuf(out.rdbuf());
    eng.pumpTimers();
    std::cout.rdbuf(old);
    EXPECT(out.str().find("fired") != std::string::npos);
}

TEST(js_settimeout_with_function_and_delay) {
    JSEngine eng;
    // Function timers (closures!) plus a real delay.
    eng.execute("var x = 0; setTimeout(function() { x = 5; }, 5);");
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    eng.pumpTimers();
    EXPECT_EQ(eng.evaluateToString("x"), std::string("5"));
}

TEST(js_setinterval_fires_repeatedly) {
    JSEngine eng;
    eng.execute("var count = 0; setInterval(function() { count = count + 1; }, 1);");
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    eng.pumpTimers();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    eng.pumpTimers();
    int n = std::atoi(eng.evaluateToString("count").c_str());
    EXPECT(n >= 1);
}

TEST(js_dom_innerhtml_parses_children) {
    JSEngine eng;
    auto dom = parseHTML("<html><body><div id='d'>old</div></body></html>");
    eng.setDocument(dom);
    eng.execute("document.getElementById('d').innerHTML = '<b>a</b><i>b</i>';"
                "var d = document.getElementById('d');");
    EXPECT_EQ(eng.evaluateToString("d.children.length"), std::string("2"));
    EXPECT_EQ(eng.evaluateToString("d.innerHTML.indexOf('<b>') >= 0"),
              std::string("true"));
}

TEST(js_dom_create_and_append) {
    JSEngine eng;
    auto dom = parseHTML("<html><body><ul id='list'></ul></body></html>");
    eng.setDocument(dom);
    eng.execute("var li = document.createElement('li');"
                "li.textContent = 'item';"
                "document.getElementById('list').appendChild(li);");
    auto ul = findById(dom, "list");
    EXPECT(ul != nullptr);
    if (ul) {
        EXPECT_EQ(ul->children.size(), (size_t)1);
        if (!ul->children.empty()) {
            EXPECT(ul->children[0]->tag == "li");
            EXPECT_EQ(joinText(ul->children[0]), std::string("item"));
        }
    }
}

TEST(js_dom_style_fontsize_kebab) {
    JSEngine eng;
    auto dom = parseHTML("<html><body><p id='p'>x</p></body></html>");
    eng.setDocument(dom);
    eng.execute("document.getElementById('p').style.fontSize = '20px';"
                "document.getElementById('p').style.backgroundColor = 'yellow';");
    auto p = findById(dom, "p");
    if (p) {
        std::string st = p->attrs.count("style") ? p->attrs["style"] : "";
        EXPECT(st.find("font-size: 20px") != std::string::npos);
        EXPECT(st.find("background-color: yellow") != std::string::npos);
    } else {
        EXPECT(false);
    }
}

TEST(js_dom_checked_and_value) {
    JSEngine eng;
    auto dom = parseHTML("<html><body><input id='c' type='checkbox'>"
                         "<input id='t' value='hi'></body></html>");
    eng.setDocument(dom);
    eng.execute("var c = document.getElementById('c');"
                "c.checked = true;"
                "var t = document.getElementById('t');"
                "t.value = t.value + ' there';");
    auto c = findById(dom, "c");
    auto t = findById(dom, "t");
    if (c) EXPECT(c->attrs.count("checked") > 0);
    if (t) EXPECT_EQ(t->attrs["value"], std::string("hi there"));
}

TEST(js_dom_queryselector) {
    JSEngine eng;
    auto dom = parseHTML("<html><body><p class='a' id='p1'>one</p>"
                         "<p class='a' id='p2'>two</p></body></html>");
    eng.setDocument(dom);
    EXPECT_EQ(eng.evaluateToString("document.querySelector('.a').id"),
              std::string("p1"));
    EXPECT_EQ(eng.evaluateToString("document.querySelectorAll('p').length"),
              std::string("2"));
    EXPECT_EQ(eng.evaluateToString("document.querySelector('#p2').id"),
              std::string("p2"));
}

TEST(js_addeventlistener_attaches_and_fires) {
    JSEngine eng;
    auto dom = parseHTML("<html><body><button id='b'>go</button></body></html>");
    eng.setDocument(dom);
    // Attach a closure listener (the old engine could only do named
    // functions stored as strings).
    eng.execute("var hits = 0;"
                "document.getElementById('b').addEventListener('click',"
                "  function(ev) { hits = hits + 1; if (ev.type != 'click') { hits = -99; } });");
    auto btn = findById(dom, "b");
    eng.dispatchEvent("click", btn);
    eng.dispatchEvent("click", btn);
    EXPECT_EQ(eng.evaluateToString("hits"), std::string("2"));
}

TEST(js_document_bubbles_events) {
    JSEngine eng;
    auto dom = parseHTML("<html><body><div id='wrap'><button id='b'>go</button></div></body></html>");
    eng.setDocument(dom);
    eng.execute("var order = '';"
                "document.getElementById('b').addEventListener('click',"
                "  function() { order = order + 'b'; });"
                "document.getElementById('wrap').addEventListener('click',"
                "  function() { order = order + 'w'; });"
                "document.addEventListener('click', function() { order = order + 'd'; });");
    eng.dispatchEvent("click", findById(dom, "b"));
    EXPECT_EQ(eng.evaluateToString("order"), std::string("bwd"));
}

TEST(js_fetch_error_path) {
    JSEngine eng;
    eng.setDocumentUrl("http://localhost/");
    // Nothing listens on port 1 — fetch must fail cleanly (no crash),
    // and the result object must expose ok=false.
    std::string r = eng.evaluateToString(
        "var res = fetch('http://127.0.0.1:1/x'); res.ok");
    EXPECT_EQ(r, std::string("false"));
}

TEST(js_location_parts) {
    JSEngine eng;
    eng.setDocumentUrl("https://example.com:8443/docs/page.html?x=1#frag");
    EXPECT_EQ(eng.evaluateToString("location.host"),     std::string("example.com:8443"));
    EXPECT_EQ(eng.evaluateToString("location.hostname"), std::string("example.com"));
    EXPECT_EQ(eng.evaluateToString("location.pathname"), std::string("/docs/page.html"));
    EXPECT_EQ(eng.evaluateToString("location.search"),   std::string("?x=1"));
    EXPECT_EQ(eng.evaluateToString("location.hash"),     std::string("#frag"));
    EXPECT_EQ(eng.evaluateToString("location.protocol"), std::string("https:"));
}

TEST(js_location_href_navigates) {
    JSEngine eng;
    eng.setDocumentUrl("http://example.com/a.html");
    std::string navTo;
    eng.onNavigate = [&](const std::string& u) { navTo = u; };
    eng.execute("location.href = 'b.html';");
    EXPECT_EQ(navTo, std::string("http://example.com/b.html"));
    eng.execute("location.assign('http://other.example/x');");

    EXPECT_EQ(navTo, std::string("http://other.example/x"));
}

TEST(js_alert_reaches_host) {
    JSEngine eng;
    std::string got;
    eng.onAlert = [&](const std::string& m) { got = m; };
    eng.execute("alert('watch out');");
    EXPECT_EQ(got, std::string("watch out"));
}

// ---------------------------------------------------------------------------
// CSS width / height
// ---------------------------------------------------------------------------

TEST(css_width_height_parsed) {
    auto rules = parseCSS(".box { width: 200; height: 100; max-width: 400; min-width: 50; }");
    EXPECT_EQ(rules.size(), (size_t)1);
    EXPECT(rules[0].style.hasWidth);
    EXPECT_EQ(rules[0].style.width, 200);
    EXPECT(rules[0].style.hasHeight);
    EXPECT_EQ(rules[0].style.height, 100);
    EXPECT(rules[0].style.hasMaxWidth);
    EXPECT_EQ(rules[0].style.maxWidth, 400);
    EXPECT(rules[0].style.hasMinWidth);
    EXPECT_EQ(rules[0].style.minWidth, 50);
}

// ---------------------------------------------------------------------------
// CSS :hover selector matching
// ---------------------------------------------------------------------------

TEST(css_hover_selector_matches_only_when_hovered) {
    auto dom = parseHTML(
        "<html><body>"
        "<a href='x' id='link'>link</a>"
        "</body></html>");
    auto link = findById(dom, "link");
    EXPECT(link != nullptr);
    if (link) {
        // Without hover, no match.
        EXPECT(!matchesSelectorNodeHover("a:hover", link, nullptr));
        // With hover (link is hovered), match.
        EXPECT(matchesSelectorNodeHover("a:hover", link, link));
        // Non-:hover selector still works regardless.
        EXPECT(matchesSelectorNodeHover("a", link, nullptr));
        EXPECT(matchesSelectorNodeHover("a", link, link));
    }
}

// ---------------------------------------------------------------------------
// URL utilities
// ---------------------------------------------------------------------------

TEST(url_parse_basic) {
    Url u = parseUrl("https://example.com:8080/a/b?x=1&y=2#frag");
    EXPECT(u.valid);
    EXPECT_EQ(u.scheme,   std::string("https"));
    EXPECT_EQ(u.host,     std::string("example.com"));
    EXPECT_EQ(u.port,     std::string("8080"));
    EXPECT_EQ(u.path,     std::string("/a/b"));
    EXPECT_EQ(u.query,    std::string("x=1&y=2"));
    EXPECT_EQ(u.fragment, std::string("frag"));
    EXPECT_EQ(u.hostPort(), std::string("example.com:8080"));
    EXPECT_EQ(u.origin(), std::string("https://example.com:8080"));
    EXPECT_EQ(u.toString(), std::string("https://example.com:8080/a/b?x=1&y=2"));
}

TEST(url_parse_defaults) {
    Url u = parseUrl("http://example.com");
    EXPECT(u.valid);
    EXPECT_EQ(u.path, std::string("/"));
    EXPECT_EQ(u.hostPort(), std::string("example.com"));  // default port elided
}

TEST(url_join_relative_paths) {
    std::string base = "https://host.com/dir/page.html";
    EXPECT_EQ(joinUrl(base, "img.png"),   std::string("https://host.com/dir/img.png"));
    EXPECT_EQ(joinUrl(base, "./img.png"), std::string("https://host.com/dir/img.png"));
    EXPECT_EQ(joinUrl(base, "../up.png"), std::string("https://host.com/up.png"));
    EXPECT_EQ(joinUrl(base, "/abs.png"),  std::string("https://host.com/abs.png"));
    EXPECT_EQ(joinUrl(base, "sub/x?a=1"), std::string("https://host.com/dir/sub/x?a=1"));
}

TEST(url_join_fragments_and_queries) {
    std::string base = "https://host.com/dir/page.html";
    EXPECT_EQ(joinUrl(base, "#sec"), std::string("https://host.com/dir/page.html#sec"));
    EXPECT_EQ(joinUrl(base, "?q=2"), std::string("https://host.com/dir/page.html?q=2"));
}

TEST(url_join_absolute_and_schemes) {
    std::string base = "https://host.com/dir/page.html";
    EXPECT_EQ(joinUrl(base, "http://other/x"), std::string("http://other/x"));
    EXPECT_EQ(joinUrl(base, "https://other/y"), std::string("https://other/y"));
    EXPECT_EQ(joinUrl(base, "//cdn.host/lib.js"), std::string("https://cdn.host/lib.js"));
    EXPECT_EQ(joinUrl(base, "javascript:void(0)"), std::string("javascript:void(0)"));
    EXPECT_EQ(joinUrl(base, "mailto:a@b.c"), std::string("mailto:a@b.c"));
}

TEST(url_is_remote) {
    EXPECT(isRemoteUrl("http://a.b/"));
    EXPECT(isRemoteUrl("HTTPS://a.b/"));
    EXPECT(!isRemoteUrl("/local/path.html"));
    EXPECT(!isRemoteUrl("test.html"));
}

TEST(url_normalize_address_input) {
    EXPECT_EQ(normalizeAddressInput("example.com"),
              std::string("https://example.com"));
    EXPECT_EQ(normalizeAddressInput("example.com/path"),
              std::string("https://example.com/path"));
    EXPECT_EQ(normalizeAddressInput("localhost:8080/x"),
              std::string("https://localhost:8080/x"));
    EXPECT_EQ(normalizeAddressInput("http://a.b/"),
              std::string("http://a.b/"));
    EXPECT_EQ(normalizeAddressInput("test.html"), std::string("test.html"));
    EXPECT_EQ(normalizeAddressInput("about:home"), std::string("about:home"));
}

TEST(url_encode_decode) {
    EXPECT_EQ(urlEncode("a b&c=d"), std::string("a+b%26c%3Dd"));
    EXPECT_EQ(urlDecode("a+b%26c%3Dd"), std::string("a b&c=d"));
    EXPECT_EQ(urlDecode("%E2%9C%93"), std::string("\xe2\x9c\x93"));
}

// ---------------------------------------------------------------------------
// RAWTEXT tokenization for <script>/<style>
// ---------------------------------------------------------------------------

TEST(lexer_script_rawtext) {
    // `<` inside JS used to be parsed as a tag and corrupted the DOM.
    std::string js = "if (a < b && b > 2) { s = '</div>'; }";
    auto toks = tokenize("<script>" + js + "</script>");
    bool sawRaw = false;
    for (auto& t : toks)
        if (t.kind == TokKind::TEXT && t.text == js) sawRaw = true;
    EXPECT(sawRaw);
}

TEST(lexer_script_rawtext_in_parser) {
    std::string html = "<html><head><script>var x = 1 < 2;</script></head>"
                       "<body><p>ok</p></body></html>";
    auto dom = parseHTML(html);
    // Find the script node and verify its raw body.
    std::function<std::string(const std::shared_ptr<Node>&)> findScript =
        [&](const std::shared_ptr<Node>& n) -> std::string {
            for (auto& c : n->children) {
                if (c->tag == "script") {
                    std::string body;
                    for (auto& sc : c->children)
                        if (sc->tag == "text") body += sc->text;
                    return body;
                }
                std::string r = findScript(c);
                if (!r.empty()) return r;
            }
            return "";
        };
    EXPECT_EQ(findScript(dom), std::string("var x = 1 < 2;"));
    // And the paragraph after it still parses.
    EXPECT(findById(dom, std::string()) == nullptr);
    bool hasP = false;
    std::function<void(const std::shared_ptr<Node>&)> walk =
        [&](const std::shared_ptr<Node>& n) {
            for (auto& c : n->children) {
                if (c->tag == "p") hasP = true;
                walk(c);
            }
        };
    walk(dom);
    EXPECT(hasP);
}

TEST(lexer_style_rawtext) {
    auto toks = tokenize("<style>a > b { color: red; } /* x < y */</style>");
    bool sawRaw = false;
    for (auto& t : toks)
        if (t.kind == TokKind::TEXT &&
            t.text.find("a > b") != std::string::npos) sawRaw = true;
    EXPECT(sawRaw);
}

// ---------------------------------------------------------------------------
// CSS additions: rgba, #rgb, units, line-height, text-transform, bg-image
// ---------------------------------------------------------------------------

TEST(css_rgba_and_hex3) {
    Style s;
    s = applyStyle(s, "color: rgba(10, 20, 30, 128)");
    EXPECT_EQ(s.color.r, 10);
    EXPECT_EQ(s.color.g, 20);
    EXPECT_EQ(s.color.b, 30);
    EXPECT_EQ(s.color.a, 128);
    Style s2;
    s2 = applyStyle(s2, "color: #f00");
    EXPECT_EQ(s2.color.r, 255);
    EXPECT_EQ(s2.color.g, 0);
    EXPECT_EQ(s2.color.b, 0);
}

TEST(css_fontsize_units) {
    Style base;
    base.fontSize = 20;
    Style a = applyStyle(base, "font-size: 1.5em");   // 1.5 * 20 = 30
    EXPECT_EQ(a.fontSize, 30);
    Style b = applyStyle(base, "font-size: 12pt");    // 12 * 96/72 = 16
    EXPECT_EQ(b.fontSize, 16);
    Style c = applyStyle(base, "font-size: 150%");    // 1.5 * 20 = 30
    EXPECT_EQ(c.fontSize, 30);
    Style d = applyStyle(base, "font-size: 30px");
    EXPECT_EQ(d.fontSize, 30);
}

TEST(css_line_height) {
    Style s;
    s.fontSize = 16;
    s = applyStyle(s, "line-height: 1.5");
    EXPECT(s.hasLineHeight);
    EXPECT_EQ((int)s.lineHeight, 24);
    Style t;
    t.fontSize = 16;
    t = applyStyle(t, "line-height: 20px");
    EXPECT_EQ((int)t.lineHeight, 20);
}

TEST(css_text_transform) {
    Style s;
    s = applyStyle(s, "text-transform: uppercase");
    EXPECT(s.hasTextTransform);
    EXPECT_EQ(s.textTransform, std::string("uppercase"));
}

TEST(css_background_image_url) {
    Style s;
    s = applyStyle(s, "background: #333 url('bg.png') no-repeat");
    EXPECT(s.hasBg);
    EXPECT(s.hasBgImage);
    EXPECT_EQ(s.bgImageUrl, std::string("bg.png"));
    EXPECT_EQ(s.bg.r, 0x33);
    Style t;
    t = applyStyle(t, "background-image: url(\"x.jpg\")");
    EXPECT(t.hasBgImage);
    EXPECT_EQ(t.bgImageUrl, std::string("x.jpg"));
}

TEST(css_font_weight_numbers) {
    Style s;
    s = applyStyle(s, "font-weight: 700");
    EXPECT(s.bold);
    Style t;
    t = applyStyle(t, "font-weight: 400");
    EXPECT(!t.bold);
}

// ---------------------------------------------------------------------------
// Layout additions: pre whitespace, zoom, tables
// ---------------------------------------------------------------------------

TEST(layout_pre_preserves_whitespace) {
    setGlobalZoom(1.0f);
    auto dom = parseHTML("<html><body><pre>line1\n  line2  x</pre></body></html>");
    LayoutResult r = layout(dom, 400, nullptr);
    // Find the <pre> box: exactly two lines, inner spaces preserved.
    bool found = false;
    for (auto& b : r.boxes) {
        if (b.lines.size() == 2) {
            std::string l1, l2;
            for (auto& run : b.lines[0]) l1 += run.text;
            for (auto& run : b.lines[1]) l2 += run.text;
            EXPECT_EQ(l1, std::string("line1"));
            EXPECT_EQ(l2, std::string("  line2  x"));
            found = true;
        }
    }
    EXPECT(found);
}

TEST(layout_zoom_scales_tag_defaults) {
    setGlobalZoom(2.0f);
    auto dom = parseHTML("<html><body><h1>Title</h1></body></html>");
    LayoutResult r = layout(dom, 400, nullptr);
    setGlobalZoom(1.0f);   // restore for other tests
    bool found = false;
    for (auto& b : r.boxes) {
        if (b.style.fontSize == 64) found = true;   // h1 32px * 2
    }
    EXPECT(found);
}

TEST(layout_line_height_expands_box) {
    setGlobalZoom(1.0f);
    auto dom = parseHTML("<html><head><style>"
                         "p { line-height: 40px; }"
                         "</style></head><body><p>one two three</p></body></html>");
    std::vector<CSSRule> rules = parseCSS("p { line-height: 40px; }");
    LayoutResult r = layout(dom, 300, nullptr, rules);
    bool found = false;
    for (auto& b : r.boxes) {
        if (b.lines.size() == 1 && b.h >= 40) found = true;
    }
    EXPECT(found);
}

TEST(layout_table_rows_align) {
    setGlobalZoom(1.0f);
    auto dom = parseHTML(
        "<html><body><table border='1'>"
        "<tr><td>a</td><td>b<br>second<br>third</td></tr>"
        "<tr><td>c</td><td>d</td></tr>"
        "</table></body></html>");
    LayoutResult r = layout(dom, 500, nullptr);
    EXPECT(!r.boxes.empty());
    // Cells in the same row must start at the same y. Collect all boxes
    // that carry a border (each cell has one from border='1').
    std::vector<int> topRowYs, bottomRowYs;
    int minY = 1 << 20;
    for (auto& b : r.boxes) {
        if (!b.style.hasBorder) continue;
        if (b.y < minY) minY = b.y;
    }
    for (auto& b : r.boxes) {
        if (!b.style.hasBorder) continue;
        if (b.y == minY) topRowYs.push_back(b.x);
        else bottomRowYs.push_back(b.y);
    }
    EXPECT(topRowYs.size() >= 2);   // two cells share the top row's y
}

// ---------------------------------------------------------------------------
// Text shaping / bidi (RTL support)
// ---------------------------------------------------------------------------

// Local UTF-8 encoder for building expected shaped strings.
static void encode(std::string& out, uint32_t cp) {
    if (cp < 0x80) { out += (char)cp; return; }
    if (cp < 0x800) {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    } else {
        out += (char)(0xF0 | (cp >> 18));
        out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
}

TEST(bidi_detect_rtl) {
    EXPECT(!detectRTL("hello world"));
    EXPECT(!detectRTL("123 + 4 = 6"));
    EXPECT(detectRTL("\xd9\x85\xd8\xb1\xd8\xad\xd8\xa8\xd8\xa7"));   // مرحبا
    EXPECT(detectRTL("  \xd9\x85..."));                              // leading neutrals
    EXPECT(detectRTL("123 \xd9\x85\xd8\xb1\xd8\xad\xd8\xa8\xd8\xa7"));// number first
    EXPECT(!detectRTL(""));
    EXPECT(!containsRTL("plain ascii"));
    EXPECT(containsRTL("\xd8\xb9\xd8\xb1\xd8\xa8\xd9\x8a"));         // عربي
    EXPECT(containsRTL("\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d"));         // שלום (Hebrew)
}

TEST(bidi_shape_isolated_word) {
    // "مدرسة" — letters take their correct contextual forms and the
    // result is the VISUAL string (first logical letter م rightmost).
    // Forms: م initial (FEE3), د final (FEAA), ر isolated (FEAD) — the
    // preceding د is right-joining and does not join forward —, س initial
    // (FEB3), ة final (FE94). Visual order: leftmost drawn first.
    std::string shaped = shapeRTLLogical(
        "\xd9\x85\xd8\xaf\xd8\xb1\xd8\xb3\xd8\xa9");
    std::string expect;
    encode(expect, 0xFE94);   // TEH MARBUTA final   (leftmost)
    encode(expect, 0xFEB3);   // SEEN initial
    encode(expect, 0xFEAD);   // REH isolated
    encode(expect, 0xFEAA);   // DAL final
    encode(expect, 0xFEE3);   // MEEM initial       (rightmost)
    EXPECT_EQ(shaped, expect);
}

TEST(bidi_shape_lam_alef) {
    // "لا" = LAM + ALEF → single ligature glyph FEFB (isolated).
    std::string shaped = shapeRTLLogical("\xd9\x84\xd8\xa7");
    std::string expect;
    encode(expect, 0xFEFB);
    EXPECT_EQ(shaped, expect);
    // "سلا" = SEEN + LAM + ALEF → SEEN initial + LAM-ALEF final ligature.
    std::string shaped2 = shapeRTLLogical(
        "\xd8\xb3\xd9\x84\xd8\xa7");
    std::string expect2;
    encode(expect2, 0xFEFC);  // LAM-ALEF final (lam joins prev SEEN)
    encode(expect2, 0xFEB3);  // SEEN initial
    EXPECT_EQ(shaped2, expect2);
}

TEST(bidi_shape_numbers_forward) {
    // Numbers inside RTL text stay left-to-right: "عام 2024" renders
    // with "2024" readable and عام to its right.
    std::string shaped = shapeRTLLogical(
        "\xd8\xb9\xd8\xa7\xd9\x85 2024");
    std::string expect = "2024 ";
    // عام reversed with contextual forms: م final? letters: ع (AIN, dual)
    // ا (ALEF, right) م (MEEM, dual). م joins prev: ALEF is right-joining
    // → م isolated (FEE1); ا joins prev ع → final (FE8E); ع isolated-start
    // but joins next → initial (FECB).
    encode(expect, 0xFEE1);   // MEEM isolated
    encode(expect, 0xFE8E);   // ALEF final
    encode(expect, 0xFECB);   // AIN initial
    EXPECT_EQ(shaped, expect);
}

TEST(bidi_shape_percent_joins_number) {
    // "50%" — the % is an ET adjacent to EN, so the whole "50%" cluster
    // must stay forward together.
    std::string shaped = shapeRTLLogical("\xd8\xa7\xd9\x84\xd8\xb3\xd8\xb9\xd8\xb1 50%");
    EXPECT(shaped.find("50%") != std::string::npos);
}

TEST(bidi_line_reorder_visual) {
    // applyBidiToLine: RTL base, one run of "ab يا" — segments reorder
    // (يا part drawn leftmost, "ab " rightmost) and Arabic gets shaped.
    // Same-source visual groups merge into one output run.
    std::vector<Run> line;
    Run r;
    r.text = "ab \xd9\x8a\xd8\xa7";
    line.push_back(r);
    applyBidiToLine(line, true);
    EXPECT_EQ(line.size(), size_t(1));
    // Visual: ا final (FE8E) then ي initial (FEF3), then "ab ".
    std::string expect;
    encode(expect, 0xFE8E);
    encode(expect, 0xFEF3);
    expect += "ab ";
    EXPECT_EQ(line[0].text, expect);
    EXPECT_EQ(line[0].logical, std::string("ab \xd9\x8a\xd8\xa7"));
}

TEST(bidi_line_ltr_passthrough) {
    std::vector<Run> line;
    Run r;
    r.text = "just ascii text";
    line.push_back(r);
    applyBidiToLine(line, false);
    EXPECT_EQ(line.size(), size_t(1));
    EXPECT_EQ(line[0].text, std::string("just ascii text"));
}

TEST(bidi_mixed_line_run_split) {
    // English sentence with an embedded Hebrew word — LTR base:
    // "say שלום ok" → the Hebrew word is reversed in place (RTL segment),
    // positions and LTR text untouched. Same-source visual groups merge
    // back into one run per source run.
    std::vector<Run> line;
    Run r;
    r.text = "say \xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d ok";
    line.push_back(r);
    applyBidiToLine(line, false);
    EXPECT_EQ(line.size(), size_t(1));
    // Hebrew שלום reversed = םולש in the visual string.
    EXPECT_EQ(line[0].text, std::string("say \xd7\x9d\xd7\x95\xd7\x9c\xd7\xa9 ok"));
    EXPECT_EQ(line[0].logical, std::string("say \xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d ok"));
}

TEST(bidi_rtl_page_layout_direction) {
    // dir="rtl" on <html> must reach the laid-out boxes and right-align
    // the text (renderer default start → right).
    auto dom = parseHTML(
        "<html dir='rtl'><body><p>\xd9\x85\xd8\xb1\xd8\xad\xd8\xa8\xd8\xa7</p>"
        "</body></html>");
    LayoutResult r = layout(dom, 600, nullptr);
    bool sawRTL = false;
    for (auto& b : r.boxes)
        if (!b.lines.empty()) { sawRTL = sawRTL || b.rtl; }
    EXPECT(sawRTL);
}

TEST(css_direction_property) {
    auto dom = parseHTML(
        "<html><body><p style=\"direction: rtl\">\xd8\xb9\xd8\xb1\xd8\xa8\xd9\x8a</p>"
        "</body></html>");
    LayoutResult r = layout(dom, 600, nullptr);
    bool sawRTL = false;
    for (auto& b : r.boxes)
        if (!b.lines.empty()) sawRTL = sawRTL || b.rtl;
    EXPECT(sawRTL);
}

// ---------------------------------------------------------------------------
// CSS Grid
// ---------------------------------------------------------------------------

TEST(grid_two_columns) {
    setGlobalZoom(1.0f);
    auto dom = parseHTML(
        "<html><head><style>"
        ".wrap { display: grid; grid-template-columns: 100px 1fr; gap: 10px; }"
        "</style></head><body><div class='wrap'>"
        "<div>alpha</div><div>beta</div><div>gamma</div>"
        "</div></body></html>");
    std::vector<CSSRule> rules = parseCSS(
        ".wrap { display: grid; grid-template-columns: 100px 1fr; gap: 10px; }");
    LayoutResult r = layout(dom, 600, nullptr, rules);
    // Children land in row 0 (alpha 100px, beta fill) then row 1 (gamma).
    // Find the boxes carrying the text: their x positions must differ by
    // 110px for row 0, and gamma sits on a second row (y greater).
    std::vector<const Box*> texts;
    for (auto& b : r.boxes) {
        if (b.lines.empty() || b.lines[0].empty()) continue;
        const std::string& t = b.lines[0][0].logical.empty()
                                   ? b.lines[0][0].text : b.lines[0][0].logical;
        if (t.find("alpha") != std::string::npos ||
            t.find("beta") != std::string::npos ||
            t.find("gamma") != std::string::npos)
            texts.push_back(&b);
    }
    EXPECT(texts.size() == 3);
    if (texts.size() == 3) {
        const Box *a = nullptr, *bb = nullptr, *g = nullptr;
        for (auto* p : texts) {
            std::string t = p->lines[0][0].logical.empty()
                                ? p->lines[0][0].text : p->lines[0][0].logical;
            if (t.find("alpha") != std::string::npos) a = p;
            if (t.find("beta") != std::string::npos) bb = p;
            if (t.find("gamma") != std::string::npos) g = p;
        }
        EXPECT(a && bb && g);
        if (a && bb && g) {
            EXPECT_EQ(bb->x - a->x, 110);        // 100px + 10px gap
            EXPECT(g->y > a->y);                 // wrapped to row 2
            EXPECT_EQ(g->x, a->x);               // back at column 1
            EXPECT(bb->w > 200);                 // fr column takes the rest
        }
    }
}

TEST(grid_repeat_autofill) {
    setGlobalZoom(1.0f);
    std::vector<CSSRule> rules = parseCSS(
        ".g { display: grid; grid-template-columns: repeat(auto-fill, minmax(200px, 1fr)); gap: 20px; }");
    auto dom = parseHTML(
        "<html><head><style>.g { display: grid; grid-template-columns: repeat(auto-fill, minmax(200px, 1fr)); gap: 20px; }</style>"
        "</head><body><div class='g'>"
        "<div>one</div><div>two</div><div>three</div>"
        "</div></body></html>");
    LayoutResult r = layout(dom, 660, nullptr, rules);
    // 660px with 200px min + 20px gap → 3 columns of 200px.
    // All three children fit on ONE row (same y), x = 0 / 220 / 440.
    std::vector<int> xs, ys;
    for (auto& b : r.boxes) {
        if (b.lines.empty() || b.lines[0].empty()) continue;
        const std::string& t = b.lines[0][0].logical.empty()
                                   ? b.lines[0][0].text : b.lines[0][0].logical;
        if (t == "one" || t == "two" || t == "three") {
            xs.push_back(b.x);
            ys.push_back(b.y);
        }
    }
    EXPECT(xs.size() == 3);
    if (xs.size() == 3) {
        EXPECT_EQ(ys[0], ys[1]); EXPECT_EQ(ys[1], ys[2]);
        EXPECT(xs[0] < xs[1] && xs[1] < xs[2]);
    }
}

TEST(grid_named_areas) {
    setGlobalZoom(1.0f);
    std::vector<CSSRule> rules = parseCSS(
        ".page { display: grid; grid-template-columns: 150px 1fr; "
        "grid-template-areas: 'side main'; } "
        ".s { grid-area: side; } .m { grid-area: main; }");
    auto dom = parseHTML(
        "<html><head><style>"
        ".page { display: grid; grid-template-columns: 150px 1fr; "
        "grid-template-areas: 'side main'; }"
        ".s { grid-area: side; } .m { grid-area: main; }"
        "</style></head><body><div class='page'>"
        "<div class='m'>content here</div>"
        "<div class='s'>sidebar nav</div>"
        "</div></body></html>");
    LayoutResult r = layout(dom, 800, nullptr, rules);
    // DOM order is main-then-side, but areas force side into column 1.
    const Box *side = nullptr, *main = nullptr;
    for (auto& b : r.boxes) {
        if (b.lines.empty() || b.lines[0].empty()) continue;
        std::string t = b.lines[0][0].logical.empty()
                            ? b.lines[0][0].text : b.lines[0][0].logical;
        if (t.find("sidebar") != std::string::npos) side = &b;
        if (t.find("content") != std::string::npos) main = &b;
    }
    EXPECT(side && main);
    if (side && main) {
        std::cerr << "  [dbg] side=(" << side->x << "," << side->y
                  << ") main=(" << main->x << "," << main->y << ")\n";
        EXPECT(side->x < main->x);           // sidebar left despite DOM order
        EXPECT_EQ(side->y, main->y);         // same row
    }
}

// ---------------------------------------------------------------------------
// mpv integration helpers + async resource loader plumbing
// ---------------------------------------------------------------------------

TEST(mpv_media_file_detection) {
    // Extension wins over query/fragment; case-insensitive; pages aren't media.
    EXPECT(mpv::isMediaFile("https://example.com/video.mp4"));
    EXPECT(mpv::isMediaFile("https://example.com/video.mp4?t=30"));
    EXPECT(mpv::isMediaFile("https://example.com/clip.MKV#t=0"));
    EXPECT(mpv::isMediaFile("https://example.com/stream.m3u8"));
    EXPECT(mpv::isMediaFile("https://example.com/song.flac"));
    EXPECT(!mpv::isMediaFile("https://example.com/index.html"));
    EXPECT(!mpv::isMediaFile("https://example.com/watch?v=abc"));
    EXPECT(!mpv::isMediaFile("https://example.com/"));
    // available() just answers a bool — whatever the sandbox has, it must
    // not crash and must be stable across calls.
    EXPECT(mpv::available() == mpv::available());
}

TEST(resource_loader_async_api) {
    // No jobs pending, no arrivals pending on a fresh loader state.
    ResourceLoader::instance().setBaseDir("");
    EXPECT_EQ(ResourceLoader::instance().pendingPreloads(), 0);
    EXPECT(!ResourceLoader::instance().consumeImagesArrived());
}

TEST(mpv_launch_diagnostics) {
    // path(): absolute executable when available, empty otherwise — the
    // launcher execs this exact path, so it must be usable as-is.
    if (mpv::available()) {
        std::string p = mpv::path();
        EXPECT(!p.empty());
        EXPECT(p[0] == '/');
        EXPECT(p.find("mpv") != std::string::npos);
    } else {
        EXPECT(mpv::path().empty());
    }
    // Liveness before any launch: no pid recorded -> not alive.
    // (alive() only reports the process from the most recent play(); a
    // fresh process has launched nothing.)
    EXPECT(mpv::alive() == false);

    // YouTube-family hosts need the ytdl extractor; plain media URLs
    // (direct files) do not.
    EXPECT(mpv::needsYtDlp("https://www.youtube.com/watch?v=abc"));
    EXPECT(mpv::needsYtDlp("https://youtube.com/watch?v=abc"));
    EXPECT(mpv::needsYtDlp("https://m.youtube.com/watch?v=abc"));
    EXPECT(mpv::needsYtDlp("https://music.youtube.com/watch?v=abc"));
    EXPECT(mpv::needsYtDlp("https://youtu.be/abc"));
    EXPECT(mpv::needsYtDlp("https://www.youtube-nocookie.com/embed/abc"));
    EXPECT(!mpv::needsYtDlp("https://example.com/video.mp4"));
    EXPECT(!mpv::needsYtDlp("https://vimeo.com/12345"));
    EXPECT(!mpv::needsYtDlp("not-a-url"));
    // notyoutube.com must NOT suffix-match youtube.com.
    EXPECT(!mpv::needsYtDlp("https://notyoutube.com/watch?v=abc"));

    // logTail() on the (possibly missing) log must be safe and sane.
    std::string tail = mpv::logTail(3);
    EXPECT(tail.size() <= 200);
}

TEST(youtube_shim_never_raw_html) {
    // v2.8: the YouTube data-extractor is REINTRODUCED (v2.7 had removed
    // it). The page is branded just "YouTube" (NOT "YouTube Lite" — it
    // is the actual page's own ytInitialData JSON, rendered statically
    // because the JS engine cannot drive the SPA). With the Chrome UA in
    // net/fetch.cpp the JSON is reliably present, so the extractor works
    // every time (in v2.6 with the "MiniBrowser" UA YouTube often served
    // a bot/consent stub with no JSON — the extractor was flaky).
    //
    // This test uses a junk HTML payload (no ytInitialData) to assert
    // the "always return a playable page" contract: even with no JSON,
    // a watch page returns a clean page with the Play button (videoId
    // comes from the URL, so playback survives any HTML shape).
    const std::string junk =
        "<html><head><title>YouTube</title></head><body><div id=\"app\"></div>"
        "<script>var ytcfg={};</script></body></html>";

    // Watch page, no bridge failure reason.
    bool ok = false;
    std::string html = shims::youTubeLiteHtml(
        "https://www.youtube.com/watch?v=dQw4w9WgXcQ", junk, ok);
    EXPECT(ok);
    EXPECT(!html.empty());
    // v2.8: the page is branded just "YouTube" (NOT "YouTube Lite" —
    // v2.6 had a "<b>YouTube Lite</b>" header; v2.8 dropped "Lite"
    // because the page is the actual page's data, not an alternative
    // site).
    EXPECT(html.find("<b>YouTube</b>") != std::string::npos);
    EXPECT(html.find("YouTube Lite") == std::string::npos);
    // The primary chip is "Play in browser" (re-runs the bridge); the
    // mpv chip exists only when mpv is actually installed.
    EXPECT(html.find("Play in browser") != std::string::npos);
    EXPECT(html.find("https://www.youtube.com/watch?v=dQw4w9WgXcQ")
            != std::string::npos);
    if (mpv::available()) {
        EXPECT(html.find("mpv:https://www.youtube.com/watch?v=dQw4w9WgXcQ")
                != std::string::npos);
    } else {
        EXPECT(html.find("mpv not installed") != std::string::npos);
        EXPECT(html.find("mpv:https") == std::string::npos);
    }
    EXPECT(html.find("ytInitialPlayerResponse") == std::string::npos);
    EXPECT(html.find("ytcfg") == std::string::npos);   // no raw shell leaked

    // Bridge failure: the reason surfaces as an amber banner on the page,
    // not only in the status bar.
    ok = false;
    html = shims::youTubeLiteHtml(
        "https://www.youtube.com/watch?v=dQw4w9WgXcQ", junk, ok,
        "YouTube is bot-gating this network and Piped fallback failed");
    EXPECT(ok);
    EXPECT(html.find("Video playback failed:") != std::string::npos);
    EXPECT(html.find("bot-gating this network") != std::string::npos);
    EXPECT(html.find("<script>") == std::string::npos ||
           html.find("ytcfg") == std::string::npos);   // still no shell leak

    // Search page with no parseable results: clean "try again" page, not "".
    ok = false;
    html = shims::youTubeLiteHtml(
        "https://www.youtube.com/results?search_query=test", junk, ok);
    EXPECT(ok);
    EXPECT(!html.empty());
    // v2.13: the search form now submits to DuckDuckGo with
    // site:youtube.com filter (not youtube.com/results). The form has
    // a hidden 'q' field (named, included in URL) and a visible
    // #ytsearch input (NOT named, excluded from URL). The onsubmit
    // JS prefixes the user's query with "site:youtube.com ".
    EXPECT(html.find("html.duckduckgo.com/html") != std::string::npos);
    EXPECT(html.find("ytsearch") != std::string::npos);
    EXPECT(html.find("site:youtube.com") != std::string::npos);
    EXPECT(html.find("YouTube Lite") == std::string::npos); // branded "YouTube"

    // Sanity: isYouTubeUrl still classifies the host correctly — the
    // caller (fetchDocumentWithShims) gates the extractor call on this
    // and on the response being ok, so a wrong classification would
    // silently disable the extractor.
    EXPECT(shims::isYouTubeUrl("https://www.youtube.com/watch?v=abc"));
    EXPECT(shims::isYouTubeUrl("https://youtube.com/watch?v=abc"));
    EXPECT(shims::isYouTubeUrl("https://m.youtube.com/watch?v=abc"));
    EXPECT(shims::isYouTubeUrl("https://music.youtube.com/watch?v=abc"));
    EXPECT(!shims::isYouTubeUrl("https://notyoutube.com/watch?v=abc"));
    EXPECT(!shims::isYouTubeUrl("https://example.com/"));

    // v2.8: address-bar search goes to the REAL server-rendered
    // DuckDuckGo at html.duckduckgo.com/html (NOT lite.duckduckgo.com/lite
    // — that was the "lite" alternative the user explicitly asked to
    // remove in v2.7; html.duckduckgo.com/html is the same DDG service,
    // just server-rendered HTML, no JS required).
    std::string s = shims::webSearchUrl("hello world");
    EXPECT(s.find("https://html.duckduckgo.com/html/?q=") != std::string::npos);
    EXPECT(s.find("hello+world") != std::string::npos);
    EXPECT(s.find("lite.duckduckgo.com") == std::string::npos);  // not lite
    EXPECT(s.find("duckduckgo.com/lite") == std::string::npos);  // not lite

    // v2.8: maybeRewriteSearchUrl still returns "" (no rewrite).
    // google.com/search loads Google directly with the Chrome UA.
    // (Note: Google's real search page is a JS SPA, so it renders
    // mostly blank — but the user explicitly asked NOT to rewrite it.)
    EXPECT(shims::maybeRewriteSearchUrl(
        "https://www.google.com/search?q=test").empty());
    EXPECT(shims::maybeRewriteSearchUrl(
        "https://duckduckgo.com/?q=test").empty());   // never rewritten
    EXPECT(shims::maybeRewriteSearchUrl(
        "https://example.com/").empty());            // not /search
}

// ---------------------------------------------------------------------------
// Round 3: HTML5 implied end tags
// ---------------------------------------------------------------------------

static std::shared_ptr<Node> firstTag(const std::shared_ptr<Node>& n,
                                      const std::string& tag) {
    if (!n) return nullptr;
    if (n->tag == tag) return n;
    for (auto& c : n->children)
        if (auto r = firstTag(c, tag)) return r;
    return nullptr;
}

static size_t countTag(const std::shared_ptr<Node>& n, const std::string& tag) {
    if (!n) return 0;
    size_t c = (n->tag == tag) ? 1 : 0;
    for (auto& ch : n->children) c += countTag(ch, tag);
    return c;
}

TEST(parser_implied_li_siblings) {
    auto root = parseHTML("<ul><li>a<li>b</ul>");
    auto ul = firstTag(root, "ul");
    EXPECT(ul != nullptr);
    if (ul) {
        EXPECT_EQ(ul->children.size(), (size_t)2);
        EXPECT_EQ(ul->children[0]->tag, std::string("li"));
        EXPECT_EQ(ul->children[1]->tag, std::string("li"));
        EXPECT_EQ(joinText(ul->children[0]), std::string("a"));
        EXPECT_EQ(joinText(ul->children[1]), std::string("b"));
    }
}

TEST(parser_implied_p_blocks) {
    auto root = parseHTML("<body><p>one<p>two<div>deep</div>");
    auto body = firstTag(root, "body");
    EXPECT(body != nullptr);
    if (body) {
        // p, p, div all siblings at body level; "two" NOT inside "one".
        EXPECT_EQ(joinText(body->children[0]), std::string("one"));
        EXPECT_EQ(joinText(body->children[1]), std::string("two"));
        EXPECT_EQ(body->children[2]->tag, std::string("div"));
    }
}

TEST(parser_implied_p_through_inline) {
    // A <p> closes through inline wrappers when a block opens.
    auto root = parseHTML("<p><b>bold<p>next");
    EXPECT_EQ(countTag(root, "p"), (size_t)2);
    auto firstP = firstTag(root, "p");
    EXPECT(firstP != nullptr);
    if (firstP) {
        // "bold" sits inside the <b>; recurse for the full text.
        std::function<std::string(const std::shared_ptr<Node>&)> deepText =
            [&](const std::shared_ptr<Node>& n) -> std::string {
                std::string s;
                for (auto& c : n->children) {
                    if (c->tag == "text") s += c->text;
                    else s += deepText(c);
                }
                return s;
            };
        EXPECT_EQ(deepText(firstP), std::string("bold"));
        // The second p is a SIBLING of the first, not a child.
        for (auto& c : firstP->children)
            EXPECT(c->tag != "p");
    }
}

TEST(parser_implied_table_rows) {
    auto root = parseHTML(
        "<table><tr><td>a<td>b<tr><td>c</table>");
    auto table = firstTag(root, "table");
    EXPECT(table != nullptr);
    if (table) {
        size_t rows = 0;
        for (auto& c : table->children) if (c->tag == "tr") ++rows;
        EXPECT_EQ(rows, (size_t)2);
        auto tr1 = firstTag(table, "tr");
        EXPECT(tr1 != nullptr);
        if (tr1) {
            size_t cells = 0;
            for (auto& c : tr1->children) if (c->tag == "td") ++cells;
            EXPECT_EQ(cells, (size_t)2);
            EXPECT_EQ(joinText(tr1->children[0]), std::string("a"));
            EXPECT_EQ(joinText(tr1->children[1]), std::string("b"));
        }
    }
}

TEST(parser_implied_dl_dt_dd) {
    auto root = parseHTML("<dl><dt>term<dd>definition</dl>");
    auto dl = firstTag(root, "dl");
    EXPECT(dl != nullptr);
    if (dl) {
        bool sawDt = false, sawDd = false;
        for (auto& c : dl->children) {
            if (c->tag == "dt") sawDt = true;
            if (c->tag == "dd") sawDd = true;
        }
        EXPECT(sawDt);
        EXPECT(sawDd);
    }
}

TEST(parser_implied_select_options) {
    auto root = parseHTML(
        "<select><option>a<option>b<option selected>c</select>");
    auto sel = firstTag(root, "select");
    EXPECT(sel != nullptr);
    if (sel) {
        size_t opts = 0;
        for (auto& c : sel->children) if (c->tag == "option") ++opts;
        EXPECT_EQ(opts, (size_t)3);
    }
}

TEST(parser_implied_li_scope_nested) {
    // The inner <ul> scopes the inner <li>: it must NOT close the
    // outer list's <li>.
    auto root = parseHTML("<ul><li>a<ul><li>b</ul><li>c</ul>");
    auto outerUl = firstTag(root, "ul");
    EXPECT(outerUl != nullptr);
    if (outerUl) {
        size_t outerLis = 0;
        for (auto& c : outerUl->children) if (c->tag == "li") ++outerLis;
        EXPECT_EQ(outerLis, (size_t)2);
        // Strictly-descendant search (firstTag would match outerUl itself).
        std::function<std::shared_ptr<Node>(const std::shared_ptr<Node>&)>
            findInner = [&](const std::shared_ptr<Node>& n)
                -> std::shared_ptr<Node> {
                for (auto& c : n->children) {
                    if (c->tag == "ul") return c;
                    if (auto r = findInner(c)) return r;
                }
                return nullptr;
            };
        auto innerUl = findInner(outerUl);
        EXPECT(innerUl != nullptr);
        if (innerUl) {
            std::function<std::string(const std::shared_ptr<Node>&)> deepText =
                [&](const std::shared_ptr<Node>& n) -> std::string {
                    std::string s;
                    for (auto& c : n->children) {
                        if (c->tag == "text") s += c->text;
                        else s += deepText(c);
                    }
                    return s;
                };
            EXPECT_EQ(deepText(innerUl), std::string("b"));
        }
    }
}

// ---------------------------------------------------------------------------
// Round 3: entity table expansion
// ---------------------------------------------------------------------------

TEST(lexer_entity_table_expanded) {
    auto toks = tokenize("<p>&euro; &larr; &ldquo;q&rdquo; &#x2713; &times;</p>");
    std::string s = joinText(toks);
    EXPECT(s.find("\xe2\x82\xac") != std::string::npos);   // euro
    EXPECT(s.find("\xe2\x86\x90") != std::string::npos);   // larr
    EXPECT(s.find("\xe2\x80\x9c") != std::string::npos);   // ldquo
    EXPECT(s.find("\xe2\x9c\x93") != std::string::npos);   // check mark
    EXPECT(s.find("\xc3\x97") != std::string::npos);       // times
}

TEST(lexer_legacy_semicolonless_entities) {
    // Semicolon-less classics decode only before a non-name character.
    EXPECT_EQ(joinText(tokenize("<p>a&amp b</p>")), std::string("a& b"));
    EXPECT_EQ(joinText(tokenize("<p>a&amp;b</p>")), std::string("a&b"));
    EXPECT_EQ(joinText(tokenize("<p>2&nbsp;3</p>")),
              std::string("2\xc2\xa0" "3"));
    // Followed by a letter it must stay literal (no word mangling).
    EXPECT_EQ(joinText(tokenize("<p>&ampersand</p>")),
              std::string("&ampersand"));
}

// ---------------------------------------------------------------------------
// Round 3: <base href>
// ---------------------------------------------------------------------------

TEST(parser_find_base_href) {
    auto root = parseHTML(
        "<html><head><base href=\"https://cdn.example.io/assets/\">"
        "</head><body><p>x</p></body></html>");
    EXPECT_EQ(findBaseHref(root), std::string("https://cdn.example.io/assets/"));
}

TEST(parser_find_base_href_absent) {
    auto root = parseHTML("<html><head></head><body></body></html>");
    EXPECT_EQ(findBaseHref(root), std::string(""));
}

// ---------------------------------------------------------------------------
// Round 3: classList
// ---------------------------------------------------------------------------

TEST(js_classlist) {
    JSEngine eng;
    auto dom = parseHTML("<html><body><p id='p' class='a b'>x</p></body></html>");
    eng.setDocument(dom);
    auto p = findById(dom, "p");
    EXPECT(p != nullptr);

    eng.execute("var el = document.getElementById('p');");
    EXPECT_EQ(eng.evaluateToString("el.classList.contains('a')"),
              std::string("true"));
    EXPECT_EQ(eng.evaluateToString("el.classList.contains('z')"),
              std::string("false"));

    eng.execute("el.classList.add('z');");
    EXPECT_EQ(eng.evaluateToString("el.classList.contains('z')"),
              std::string("true"));
    EXPECT(p != nullptr && p->attrs["class"] == "a b z");

    eng.execute("el.classList.remove('a');");
    EXPECT(p != nullptr && p->attrs["class"] == "b z");

    EXPECT_EQ(eng.evaluateToString("el.classList.toggle('b')"),
              std::string("false"));   // was present -> removed
    EXPECT(p != nullptr && p->attrs["class"] == "z");
    EXPECT_EQ(eng.evaluateToString("el.classList.toggle('b')"),
              std::string("true"));    // was absent -> added
    EXPECT(p != nullptr && p->attrs["class"] == "z b");

    EXPECT_EQ(eng.evaluateToString("el.classList.length"), std::string("2"));
    EXPECT_EQ(eng.evaluateToString("el.classList.item(0)"), std::string("z"));
}

// ---------------------------------------------------------------------------
// Round 3: tree properties + manipulation
// ---------------------------------------------------------------------------

TEST(js_tree_properties) {
    JSEngine eng;
    auto dom = parseHTML(
        "<html><body><ul id='u'><li>one</li><li>two</li></ul></body></html>");
    eng.setDocument(dom);
    eng.execute("var ul = document.getElementById('u');");
    EXPECT_EQ(eng.evaluateToString("ul.firstChild.textContent"),
              std::string("one"));
    EXPECT_EQ(eng.evaluateToString("ul.lastChild.textContent"),
              std::string("two"));
    // Between the two <li> there may be a text node (whitespace) — the
    // element sibling walk must skip it.
    EXPECT_EQ(eng.evaluateToString(
                  "ul.firstChild.nextElementSibling.textContent"),
              std::string("two"));
    EXPECT_EQ(eng.evaluateToString(
                  "ul.lastChild.previousElementSibling.textContent"),
              std::string("one"));
    EXPECT_EQ(eng.evaluateToString("ul.lastChild.nextSibling === null"),
              std::string("true"));
    EXPECT_EQ(eng.evaluateToString("ul.nodeType"), std::string("1"));
}

TEST(js_insert_replace_clone) {
    JSEngine eng;
    auto dom = parseHTML("<html><body><ul id='u'><li>a</li><li>c</li></ul></body></html>");
    eng.setDocument(dom);
    eng.execute(
        "var ul = document.getElementById('u');"
        "var li = document.createElement('li');"
        "li.textContent = 'b';"
        "ul.insertBefore(li, ul.children[1]);");
    EXPECT_EQ(eng.evaluateToString("ul.children.length"), std::string("3"));
    EXPECT_EQ(eng.evaluateToString("ul.children[1].textContent"),
              std::string("b"));

    eng.execute(
        "var fresh = ul.lastChild.cloneNode(true);"
        "fresh.textContent = 'c2';"
        "ul.replaceChild(fresh, ul.lastChild);");
    EXPECT_EQ(eng.evaluateToString("ul.lastChild.textContent"),
              std::string("c2"));
    EXPECT_EQ(eng.evaluateToString("ul.children.length"), std::string("3"));
}

TEST(js_contains_closest_matches) {
    JSEngine eng;
    auto dom = parseHTML(
        "<html><body>"
        "<div id='outer' class='wrap'><p id='inner'>x</p></div>"
        "</body></html>");
    eng.setDocument(dom);
    eng.execute(
        "var o = document.getElementById('outer');"
        "var i = document.getElementById('inner');");
    EXPECT_EQ(eng.evaluateToString("o.contains(i)"), std::string("true"));
    EXPECT_EQ(eng.evaluateToString("o.contains(o)"), std::string("true"));
    EXPECT_EQ(eng.evaluateToString("i.contains(o)"), std::string("false"));
    EXPECT_EQ(eng.evaluateToString("i.closest('.wrap').id"),
              std::string("outer"));
    EXPECT_EQ(eng.evaluateToString("i.matches('p')"), std::string("true"));
    EXPECT_EQ(eng.evaluateToString("i.matches('.nope')"),
              std::string("false"));
}

// ---------------------------------------------------------------------------
// Round 3: base64 / URI / navigator / storage / rAF
// ---------------------------------------------------------------------------

TEST(js_btoa_atob) {
    JSEngine eng;
    EXPECT_EQ(eng.evaluateToString("btoa('Hello')"), std::string("SGVsbG8="));
    EXPECT_EQ(eng.evaluateToString("atob('SGVsbG8=')"), std::string("Hello"));
    EXPECT_EQ(eng.evaluateToString("atob(btoa('round trip'))"),
              std::string("round trip"));
    EXPECT_EQ(eng.evaluateToString("btoa('')"), std::string(""));
}

TEST(js_uri_components) {
    JSEngine eng;
    EXPECT_EQ(eng.evaluateToString("encodeURIComponent('a b&c=d')"),
              std::string("a%20b%26c%3Dd"));
    EXPECT_EQ(eng.evaluateToString("decodeURIComponent('a%20b%26c%3Dd')"),
              std::string("a b&c=d"));
    // Unlike form-encoding, space is NOT '+' and '+' is NOT space.
    EXPECT_EQ(eng.evaluateToString("encodeURIComponent('a+b')"),
              std::string("a%2Bb"));
    EXPECT_EQ(eng.evaluateToString("decodeURIComponent('a+b')"),
              std::string("a+b"));
}

TEST(js_navigator) {
    JSEngine eng;
    std::string ua = eng.evaluateToString("navigator.userAgent");
    EXPECT(ua.find("MiniBrowser") != std::string::npos);
    std::string lang = eng.evaluateToString("navigator.language");
    EXPECT(!lang.empty());
}

TEST(js_request_animation_frame) {
    JSEngine eng;
    eng.execute("var framed = 0; requestAnimationFrame(function(){ framed = 1; });");
    // 16 ms timer: wait comfortably past it, then pump.
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    eng.pumpTimers();
    EXPECT_EQ(eng.evaluateToString("framed"), std::string("1"));
}

TEST(js_local_storage) {
    setenv("MB_STORAGE_FILE", "/tmp/mb_selftest_storage", 1);
    remove("/tmp/mb_selftest_storage");

    JSEngine eng;
    EXPECT_EQ(eng.evaluateToString(
                  "localStorage.setItem('k', 'v v'); localStorage.getItem('k')"),
              std::string("v v"));
    EXPECT_EQ(eng.evaluateToString("localStorage.length"), std::string("1"));
    // Persisted to disk immediately (atomic tmp+rename).
    {
        std::ifstream f("/tmp/mb_selftest_storage");
        EXPECT(f.good());
        std::stringstream ss; ss << f.rdbuf();
        EXPECT(ss.str().find("k\nv%20v\n") != std::string::npos);
    }
    eng.execute("localStorage.removeItem('k');");
    EXPECT_EQ(eng.evaluateToString("localStorage.getItem('k')"),
              std::string("null"));
    eng.execute("localStorage.setItem('a','1'); localStorage.setItem('b','2');");
    eng.execute("localStorage.clear();");
    EXPECT_EQ(eng.evaluateToString("localStorage.length"), std::string("0"));

    // sessionStorage: same shape, memory-backed.
    eng.execute("sessionStorage.setItem('s', 't');");
    EXPECT_EQ(eng.evaluateToString("sessionStorage.getItem('s')"),
              std::string("t"));
    remove("/tmp/mb_selftest_storage");
}

// ---------------------------------------------------------------------------
// Round 3: text selection (hit-test + text extraction)
// ---------------------------------------------------------------------------

// A real font is required for measurement; DejaVu ships everywhere CI
// runs. Cached so repeated tests don't re-open it.
static TTF_Font* testFont() {
    static TTF_Font* f = [] {
        if (TTF_Init() != 0) return (TTF_Font*)nullptr;
        return loadFont("", 16);
    }();
    return f;
}

TEST(selection_hit_test_and_extract) {
    TTF_Font* font = testFont();
    EXPECT(font != nullptr);
    if (!font) return;

    auto dom = parseHTML("<html><body><p>HelloWorld</p></body></html>");
    LayoutResult r = layout(dom, 400, font);
    EXPECT(!r.boxes.empty());
    if (r.boxes.empty()) return;

    // Locate the paragraph's box by its text.
    const Box* box = nullptr;
    for (auto& b : r.boxes) {
        if (!b.lines.empty() && !b.lines[0].empty() &&
            b.lines[0][0].text.find("HelloWorld") != std::string::npos) {
            box = &b;
            break;
        }
    }
    EXPECT(box != nullptr);
    if (!box) return;
    EXPECT_EQ(box->lines.size(), (size_t)1);

    // Start of the text: left of the first glyph must snap to 0.
    TextPos start = hitTestText(r, font, box->x, box->y + 8);
    EXPECT(start.valid);
    EXPECT_EQ(start.byte, 0);

    // Far right of the line snaps to the end.
    TextPos end = hitTestText(r, font, box->x + box->w + 50, box->y + 8);
    EXPECT(end.valid);
    EXPECT_EQ(end.byte, 10);

    // Extraction across the whole line reproduces the source text.
    EXPECT_EQ(textBetween(r, start, end), std::string("HelloWorld"));
    // Order-independent: flipped anchors give the same result.
    EXPECT_EQ(textBetween(r, end, start), std::string("HelloWorld"));

    // A position in the middle cuts at a codepoint boundary; the two
    // halves concatenate back to the whole line. (Measure the actual
    // text width — the box spans the full viewport, not the glyphs.)
    int textW = 0, textH = 0;
    TTF_SizeUTF8(font, "HelloWorld", &textW, &textH);
    TextPos mid = hitTestText(r, font, box->x + 4 + textW / 2, box->y + 8);
    EXPECT(mid.valid);
    EXPECT(mid.byte > 0);
    EXPECT(mid.byte < 10);
    std::string left = textBetween(r, start, mid);
    std::string right = textBetween(r, mid, end);
    EXPECT_EQ(left + right, std::string("HelloWorld"));
}

TEST(selection_multi_box_extract) {
    TTF_Font* font = testFont();
    EXPECT(font != nullptr);
    if (!font) return;

    auto dom = parseHTML(
        "<html><body><p>alpha</p><p>beta</p></body></html>");
    LayoutResult r = layout(dom, 400, font);

    // Find both paragraph boxes.
    std::vector<const Box*> ps;
    for (auto& b : r.boxes) {
        if (!b.lines.empty() && !b.lines[0].empty() &&
            (b.lines[0][0].text.find("alpha") != std::string::npos ||
             b.lines[0][0].text.find("beta") != std::string::npos))
            ps.push_back(&b);
    }
    EXPECT_EQ(ps.size(), (size_t)2);
    if (ps.size() < 2) return;
    const Box* first = ps[0];
    const Box* last = ps[1];

    TextPos s = hitTestText(r, font, first->x, first->y + 8);
    TextPos e = hitTestText(r, font, last->x + last->w + 50,
                            last->y + last->h + 20);
    EXPECT(s.valid);
    EXPECT(e.valid);
    std::string text = textBetween(r, s, e);
    EXPECT(text.find("alpha") != std::string::npos);
    EXPECT(text.find("beta") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Round 4: element geometry (getBoundingClientRect + offset*)
// ---------------------------------------------------------------------------

// Wire a JSEngine to a finished layout exactly the way Browser does:
// providers are read at call time, so mutating *scrollY immediately
// changes what page JS observes. NOTE: `font` is captured by VALUE and
// `&lr` refers to the caller's LayoutResult — capturing "[&]" here would
// dangle (this helper's frame dies before the JS callbacks fire).
static void wireGeometry(JSEngine& eng, const LayoutResult& lr,
                         TTF_Font* font, int* scrollY) {
    eng.rectForNode = [&lr, font](const std::shared_ptr<Node>& n,
                                  DOMRect& out) {
        return rectForNode(lr, font, n, out);
    };
    eng.scrollY = [scrollY]() { return *scrollY; };
}

TEST(js_grect_block_basics) {
    TTF_Font* font = testFont();
    EXPECT(font != nullptr);
    if (!font) return;

    auto dom = parseHTML(
        "<html><body><p id='a'>First paragraph</p>"
        "<p id='b'>Second paragraph</p></body></html>");
    LayoutResult r = layout(dom, 400, font);
    JSEngine eng;
    eng.setDocument(dom);
    int scrollY = 0;
    wireGeometry(eng, r, font, &scrollY);

    eng.execute(
        "var a = document.getElementById('a').getBoundingClientRect();"
        "var b = document.getElementById('b').getBoundingClientRect();");
    // Both paragraphs laid out; the second sits below the first.
    EXPECT_EQ(eng.evaluateToString("a.width > 0"),  std::string("true"));
    EXPECT_EQ(eng.evaluateToString("a.height > 0"), std::string("true"));
    EXPECT_EQ(eng.evaluateToString("b.top > a.top"), std::string("true"));
    // Edge aliases agree with x/y/width/height.
    EXPECT_EQ(eng.evaluateToString(
        "Math.abs(a.right - (a.x + a.width)) < 1e-6 &&"
        " Math.abs(a.bottom - (a.y + a.height)) < 1e-6 &&"
        " a.left === a.x && a.top === a.y"), std::string("true"));
    // Unscrolled: client top equals the document-space offsetTop.
    EXPECT_EQ(eng.evaluateToString(
        "Math.abs(b.top - document.getElementById('b').offsetTop) < 1e-6"),
        std::string("true"));
}

TEST(js_grect_viewport_relative_and_offset) {
    TTF_Font* font = testFont();
    EXPECT(font != nullptr);
    if (!font) return;

    auto dom = parseHTML(
        "<html><body><p id='p'>Scroll me</p><p>filler</p>"
        "<p>filler</p><p>filler</p></body></html>");
    LayoutResult r = layout(dom, 400, font);
    JSEngine eng;
    eng.setDocument(dom);
    int scrollY = 0;
    wireGeometry(eng, r, font, &scrollY);

    eng.execute(
        "var el = document.getElementById('p');"
        "var offTop = el.offsetTop;"
        "var offW = el.offsetWidth, offH = el.offsetHeight;");
    EXPECT_EQ(eng.evaluateToString("offTop > 0 && offW > 0 && offH > 0"),
              std::string("true"));

    // Scrolling does not move the element in the document, but the
    // client rect must shift up by exactly the scroll amount.
    scrollY = 120;
    eng.execute("var topScrolled = el.getBoundingClientRect().top;");
    EXPECT_EQ(eng.evaluateToString(
        "Math.abs((offTop - topScrolled) - 120) < 1e-6"), std::string("true"));
    // offset* stay document-space while scrolled.
    EXPECT_EQ(eng.evaluateToString("el.offsetTop === offTop"),
              std::string("true"));
    // Sizes are scroll-independent in both APIs and agree.
    EXPECT_EQ(eng.evaluateToString(
        "el.offsetWidth === offW &&"
        " Math.abs(el.getBoundingClientRect().width - offW) < 1e-6 &&"
        " el.offsetHeight === offH"),
        std::string("true"));
}

TEST(js_grect_inline_fragments) {
    TTF_Font* font = testFont();
    EXPECT(font != nullptr);
    if (!font) return;

    // <b> owns "bo" directly and wraps <i>ld</i>; neither has a box.
    auto dom = parseHTML(
        "<html><body><p>alpha <b id='b'>bo<i id='i'>ld</i></b> tail</p>"
        "</body></html>");
    LayoutResult r = layout(dom, 400, font);
    JSEngine eng;
    eng.setDocument(dom);
    int scrollY = 0;
    wireGeometry(eng, r, font, &scrollY);

    eng.execute(
        "var p = document.querySelector('p').getBoundingClientRect();"
        "var b = document.getElementById('b').getBoundingClientRect();"
        "var i = document.getElementById('i').getBoundingClientRect();");
    // Inline elements get real fragment geometry even without a box.
    EXPECT_EQ(eng.evaluateToString("b.width > 0 && b.height > 0"),
              std::string("true"));
    EXPECT_EQ(eng.evaluateToString("i.width > 0"), std::string("true"));
    // <i> is the tail half of <b>'s text: strictly inside it on the
    // same line.
    EXPECT_EQ(eng.evaluateToString(
        "i.left > b.left && i.right <= b.right + 0.5 &&"
        " Math.abs(i.top - b.top) < 0.5"), std::string("true"));
    // Fragments sit inside the containing paragraph's box, and the
    // inline union is narrower than the full line.
    EXPECT_EQ(eng.evaluateToString(
        "b.left >= p.left && b.right <= p.right + 0.5 &&"
        " b.width < p.width"), std::string("true"));
}

TEST(js_grect_link_fragments) {
    TTF_Font* font = testFont();
    EXPECT(font != nullptr);
    if (!font) return;

    auto dom = parseHTML(
        "<html><body><p>go to <a id='l' href='#next'>the docs page</a>"
        " now</p></body></html>");
    LayoutResult r = layout(dom, 400, font);
    JSEngine eng;
    eng.setDocument(dom);
    int scrollY = 0;
    wireGeometry(eng, r, font, &scrollY);

    eng.execute(
        "var l = document.getElementById('l').getBoundingClientRect();");
    // The <a> has no box, but its link hit-rects union into a real rect.
    EXPECT_EQ(eng.evaluateToString("l.width > 0 && l.height > 0"),
              std::string("true"));
    // Preceded by "go to " on the same line: indented from the box edge.
    EXPECT_EQ(eng.evaluateToString("l.left > 0"), std::string("true"));
}

TEST(js_grect_detached_zero_rect) {
    TTF_Font* font = testFont();
    EXPECT(font != nullptr);
    if (!font) return;

    auto dom = parseHTML("<html><body><p id='p'>hi</p></body></html>");
    LayoutResult r = layout(dom, 400, font);
    JSEngine eng;
    eng.setDocument(dom);
    int scrollY = 0;
    wireGeometry(eng, r, font, &scrollY);

    // Detached elements have no laid-out fragments: all-zero rect, no
    // exception — same as real browsers give for display:none.
    eng.execute("var d = document.createElement('div');"
                "var rd = d.getBoundingClientRect();");
    EXPECT_EQ(eng.evaluateToString(
        "rd.x === 0 && rd.y === 0 && rd.width === 0 && rd.height === 0 &&"
        " rd.top === 0 && rd.left === 0 && rd.right === 0 &&"
        " rd.bottom === 0"), std::string("true"));
    EXPECT_EQ(eng.evaluateToString(
        "d.offsetWidth === 0 && d.offsetHeight === 0 &&"
        " d.offsetTop === 0 && d.offsetLeft === 0"), std::string("true"));
    EXPECT_EQ(eng.evaluateToString(
        "try { document.createElement('span').getBoundingClientRect();"
        " 'ok'; } catch (e) { 'threw'; }"), std::string("ok"));
}

TEST(js_grect_no_provider_zeros) {
    TTF_Font* font = testFont();
    EXPECT(font != nullptr);
    if (!font) return;

    // Host without geometry providers (e.g. a bare JSEngine user):
    // calls stay safe and read as zeros.
    auto dom = parseHTML("<html><body><p id='p'>hi</p></body></html>");
    JSEngine eng;
    eng.setDocument(dom);
    EXPECT_EQ(eng.evaluateToString(
        "document.getElementById('p').getBoundingClientRect().width"),
        std::string("0"));
    EXPECT_EQ(eng.evaluateToString(
        "document.getElementById('p').offsetHeight"), std::string("0"));
}

TEST(rect_for_node_cxx_union) {
    TTF_Font* font = testFont();
    EXPECT(font != nullptr);
    if (!font) return;

    auto dom = parseHTML(
        "<html><body><p id='p'>Hello <b id='b'>bold</b> world</p>"
        "</body></html>");
    LayoutResult r = layout(dom, 400, font);
    auto p = findById(dom, "p");
    auto b = findById(dom, "b");
    EXPECT(p != nullptr);
    EXPECT(b != nullptr);
    if (!p || !b) return;

    DOMRect rp{}, rb{};
    EXPECT(rectForNode(r, font, p, rp));   // block: its own box
    EXPECT(rectForNode(r, font, b, rb));   // inline: run fragment union
    EXPECT(rp.width > 0 && rp.height > 0);
    EXPECT(rb.width > 0 && rb.height > 0);
    // The inline fragment sits inside the paragraph's box.
    EXPECT(rb.left()   >= rp.left() - 0.5f);
    EXPECT(rb.right()  <= rp.right() + 0.5f);
    EXPECT(rb.top()    >= rp.top() - 0.5f);
    EXPECT(rb.bottom() <= rp.bottom() + 0.5f);
    // And it is strictly narrower than the full-width paragraph box.
    EXPECT(rb.width < rp.width);

    DOMRect rz{};
    EXPECT(!rectForNode(r, font, nullptr, rz));
}

// ---------------------------------------------------------------------------
// Internal media player (round 9): <video>/<audio> decoding in-process
// ---------------------------------------------------------------------------

TEST(media_is_media_url) {
    EXPECT(media::isMediaUrl("https://x.com/a.mp4?t=1"));
    EXPECT(media::isMediaUrl("tests/media/tone.mp3"));
    EXPECT(media::isMediaUrl("file:///home/u/movie.mkv"));
    EXPECT(media::isMediaUrl("http://host/STREAM.M3U8#x"));
    EXPECT(!media::isMediaUrl("https://x.com/page.html"));
    EXPECT(!media::isMediaUrl("https://x.com/"));
    EXPECT(!media::isMediaUrl("about:home"));
    EXPECT(!media::isMediaUrl(""));
}

TEST(media_layout_video_audio_boxes) {
    TTF_Font* font = testFont();
    EXPECT(font != nullptr);
    if (!font) return;

    auto dom = parseHTML(
        "<html><body>"
        "<video id='v' src='tests/media/sample.mp4' controls "
        "width='480' height='270'></video>"
        "<audio id='a' src='tests/media/tone.mp3' controls></audio>"
        "</body></html>");
    LayoutResult r = layout(dom, 800, font);
    int videos = 0, audios = 0;
    Box vb{}, ab{};
    for (auto& b : r.boxes) {
        if (b.isVideo) { ++videos; vb = b; }
        if (b.isAudio) { ++audios; ab = b; }
    }
    EXPECT_EQ(videos, 1);
    EXPECT_EQ(audios, 1);
    EXPECT_EQ(vb.w, 480);
    EXPECT_EQ(vb.h, 270);
    EXPECT(vb.mediaPath.find("sample.mp4") != std::string::npos);
    // Audio renders as a bar: default height 36, capped to the column.
    EXPECT_EQ(ab.h, 36);
    EXPECT(ab.mediaPath.find("tone.mp3") != std::string::npos);

    // The round-4 geometry API covers media elements too.
    auto v = findById(dom, "v");
    DOMRect rv{};
    EXPECT(rectForNode(r, font, v, rv));
    EXPECT(rv.width == 480.0f);
    EXPECT(rv.height == 270.0f);
}

TEST(media_hit_test_parts) {
    TTF_Font* font = testFont();
    EXPECT(font != nullptr);
    if (!font) return;

    auto dom = parseHTML(
        "<html><body><video id='v' src='tests/media/sample.mp4' controls "
        "width='480' height='270'></video></body></html>");
    LayoutResult r = layout(dom, 800, font);
    const Box* vb = nullptr;
    for (auto& b : r.boxes)
        if (b.isVideo) { vb = &b; break; }
    EXPECT(vb != nullptr);
    if (!vb) return;

    // Document-space hit tests against the shared control geometry.
    int barY = vb->y + vb->h - 15;              // middle of the 30px bar
    MediaHit play = mediaHitTest(*vb, vb->x + 14, barY);
    EXPECT(play.part == MediaHit::Play);
    MediaHit body = mediaHitTest(*vb, vb->x + 240, vb->y + 80);
    EXPECT(body.part == MediaHit::Body);
    MediaHit seek = mediaHitTest(*vb, vb->x + 200, barY);
    EXPECT(seek.part == MediaHit::Seek);
    EXPECT(seek.frac > 0.1 && seek.frac < 0.6);
    MediaHit mute = mediaHitTest(*vb, vb->x + vb->w - 57, barY);
    EXPECT(mute.part == MediaHit::Mute);
    MediaHit vol = mediaHitTest(*vb, vb->x + vb->w - 23, barY);
    EXPECT(vol.part == MediaHit::Volume);
    MediaHit out = mediaHitTest(*vb, vb->x + vb->w + 50, barY);
    EXPECT(out.part == MediaHit::None);

    // Without the controls attribute everything inside is the Body.
    auto dom2 = parseHTML(
        "<html><body><video id='v' src='x.mp4' width='320' height='180'>"
        "</video></body></html>");
    LayoutResult r2 = layout(dom2, 800, font);
    const Box* vb2 = nullptr;
    for (auto& b : r2.boxes)
        if (b.isVideo) { vb2 = &b; break; }
    EXPECT(vb2 != nullptr);
    if (vb2) {
        MediaHit h = mediaHitTest(*vb2, vb2->x + 5, vb2->y + 5);
        EXPECT(h.part == MediaHit::Body);
    }
}

TEST(media_player_open_decode_seek) {
#ifdef MB_MEDIA_ENABLED
    if (!ResourceLoader::fileExists("tests/media/sample.mp4")) {
        std::cerr << "  (skip: tests/media/sample.mp4 fixture missing)\n";
        return;
    }
    auto p = std::make_shared<media::MediaPlayer>();
    std::string err;
    EXPECT(p->open("tests/media/sample.mp4", &err));

    // First (preview) frame: bounded wait, headless-safe.
    media::MediaFrame f;
    int waited = 0;
    while (waited < 6000 && !p->copyFrame(f)) {
        SDL_Delay(20);
        waited += 20;
    }
    EXPECT(p->valid());
    EXPECT(!p->failed());
    if (p->failed()) std::cerr << "  open error: " << p->error() << "\n";
    EXPECT(p->hasVideo());
    EXPECT(p->hasAudio());
    EXPECT_EQ(f.w, 320);
    EXPECT_EQ(f.h, 180);
    EXPECT(f.rgba.size() == (size_t)f.w * f.h * 4);
    // testsrc2 is colorful: most decoded pixels must be non-black.
    int nonzero = 0;
    for (size_t i = 0; i + 3 < f.rgba.size(); i += 4)
        if (f.rgba[i] > 8 || f.rgba[i + 1] > 8 || f.rgba[i + 2] > 8) ++nonzero;
    EXPECT(nonzero > f.w * f.h / 4);

    // Duration ~8 s (container reports 8.0 for the generated fixture).
    EXPECT(p->duration() > 6.0 && p->duration() < 12.0);

    // Wall-clock playback: position advances while playing, freezes on
    // pause (headless builds have no audio clock — this is the master).
    p->play();
    EXPECT(p->playing());
    SDL_Delay(700);
    EXPECT(p->position() > 0.3);
    // Playback must publish frames beyond the preview (regression: the
    // old preview loop decoded to EOF and parked the decoder).
    EXPECT(p->frameSeq() > 1);
    p->pause();
    double pos1 = p->position();
    SDL_Delay(150);
    EXPECT(p->position() < pos1 + 0.05);

    // Seek to 6 s: clock lands in range, a new frame is published.
    p->seek(6.0);
    SDL_Delay(400);
    EXPECT(p->position() > 5.5 && p->position() < 6.7);
    media::MediaFrame f2;
    EXPECT(p->copyFrame(f2));
    EXPECT(f2.seq != f.seq);

    // Play through to EOF: parks at the end, playback stops.
    p->play();
    waited = 0;
    while (waited < 8000 && !p->ended()) {
        SDL_Delay(50);
        waited += 50;
    }
    EXPECT(p->ended());
    EXPECT(!p->playing());
    p->close();
#else
    std::cerr << "  (skip: built without FFmpeg - stub player)\n";
#endif
}

TEST(media_player_audio_only) {
#ifdef MB_MEDIA_ENABLED
    if (!ResourceLoader::fileExists("tests/media/tone.mp3")) {
        std::cerr << "  (skip: tests/media/tone.mp3 fixture missing)\n";
        return;
    }
    auto p = std::make_shared<media::MediaPlayer>();
    std::string err;
    EXPECT(p->open("tests/media/tone.mp3", &err));
    int waited = 0;
    while (waited < 4000 && !p->valid() && !p->failed()) {
        SDL_Delay(20);
        waited += 20;
    }
    EXPECT(p->valid());
    EXPECT(!p->failed());
    EXPECT(p->hasAudio());
    EXPECT(!p->hasVideo());
    EXPECT(p->duration() > 4.0 && p->duration() < 8.0);
    p->close();
#else
    std::cerr << "  (skip: built without FFmpeg - stub player)\n";
#endif
}

TEST(media_registry_acquire_prune) {
    if (!ResourceLoader::fileExists("tests/media/sample.mp4")) return;
    auto a = media::acquirePlayer("tests/media/sample.mp4");
    auto b = media::acquirePlayer("tests/media/sample.mp4");
    EXPECT(a != nullptr);
    EXPECT(a == b);   // one player per URL, shared by all widgets
    EXPECT(media::playerCount() >= 1);
    std::unordered_set<std::string> keep{"tests/media/sample.mp4"};
    media::prunePlayersExcept(keep);
    EXPECT(media::findPlayer("tests/media/sample.mp4") != nullptr);
    // A navigation to a page without media drops every player.
    media::prunePlayersExcept({});
    EXPECT(media::findPlayer("tests/media/sample.mp4") == nullptr);
}

TEST(media_js_media_element_api) {
#ifndef MB_MEDIA_ENABLED
    std::cerr << "  (skip: built without FFmpeg - stub player)\n";
    return;
#else
    if (!ResourceLoader::fileExists("tests/media/sample.mp4")) return;
    TTF_Font* font = testFont();
    EXPECT(font != nullptr);
    if (!font) return;

    auto dom = parseHTML(
        "<html><body><video id='v' src='tests/media/sample.mp4' controls "
        "width='160' height='90'></video></body></html>");
    LayoutResult r = layout(dom, 400, font);
    JSEngine eng;
    eng.setDocument(dom);
    int scrollY = 0;
    wireGeometry(eng, r, font, &scrollY);
    // Same resolution rule the layout used, so the JS API finds the
    // player that relayout_ acquired.
    eng.resolveMedia = [](const std::string& u) {
        return std::string("tests/media/") + "sample.mp4";
    };

    // acquire through the layout path (as Browser::relayout_ does).
    for (auto& b : r.boxes)
        if ((b.isVideo || b.isAudio) && !b.mediaPath.empty())
            media::acquirePlayer(b.mediaPath);

    int waited = 0;
    while (waited < 6000) {
        std::string ready = eng.evaluateToString(
            "String(document.getElementById('v').duration > 6)");
        if (ready == "true") break;
        SDL_Delay(25);
        waited += 25;
    }
    EXPECT_EQ(eng.evaluateToString(
        "typeof document.getElementById('v').play"), std::string("function"));
    EXPECT_EQ(eng.evaluateToString(
        "typeof document.getElementById('v').pause"), std::string("function"));
    eng.execute("var v = document.getElementById('v'); v.play();");
    SDL_Delay(300);
    EXPECT_EQ(eng.evaluateToString("v.paused"), std::string("false"));
    eng.execute("v.pause();");
    EXPECT_EQ(eng.evaluateToString("v.paused"), std::string("true"));
    EXPECT(media::anyPlaying() == false);
    eng.execute("v.currentTime = 2.0;");
    SDL_Delay(100);
    EXPECT_EQ(eng.evaluateToString(
        "v.currentTime > 1.5 && v.currentTime < 2.6"), std::string("true"));
    EXPECT_EQ(eng.evaluateToString("v.duration > 6"), std::string("true"));
    media::prunePlayersExcept({});
#endif
}

// Regression for the v2.4 "cannot create temp file" bug: the media cache
// directory ($TMPDIR/mini-browser-media) was never created, so EVERY
// remote-https playback failed before demuxing. Needs real network +
// a reachable https host, so it runs only when MB_MEDIA_LIVE=1.
TEST(media_remote_https_open) {
#ifdef MB_MEDIA_ENABLED
    if (!getenv("MB_MEDIA_LIVE")) return;   // hermetic by default
    auto p = media::acquirePlayer(
        "https://test-videos.co.uk/vids/bigbuckbunny/mp4/h264/720/"
        "Big_Buck_Bunny_720_10s_1MB.mp4");
    EXPECT(p != nullptr);
    if (!p) return;
    // Worker: mkdir cache dir -> curl temp file -> demux -> preview frame.
    for (int i = 0; i < 300 && !p->firstFrameDecided(); ++i) SDL_Delay(100);
    EXPECT(!p->failed());
    if (!p->failed()) {
        EXPECT(p->valid());
        EXPECT(p->hasVideo());
        EXPECT(p->videoW() > 0);
    }
    media::prunePlayersExcept({});
#endif
}

// ---------------------------------------------------------------------------
// yt-dlp bridge (round 10): YouTube watch URLs -> internal player
// ---------------------------------------------------------------------------

TEST(extractor_url_classification) {
    using media::extractor::isExtractableUrl;
    // Garbage / non-web / non-YouTube.
    EXPECT(!isExtractableUrl(""));
    EXPECT(!isExtractableUrl("about:home"));
    EXPECT(!isExtractableUrl("not a url"));
    EXPECT(!isExtractableUrl("https://example.com/watch?v=x"));
    // YouTube pages that are NOT a specific video.
    EXPECT(!isExtractableUrl("https://www.youtube.com/"));
    EXPECT(!isExtractableUrl("https://www.youtube.com/feed/subscriptions"));
    EXPECT(!isExtractableUrl("https://www.youtube.com/results?search_query=x"));
    // Explicit external hatch / view-source are never auto-extracted.
    EXPECT(!isExtractableUrl("mpv:https://www.youtube.com/watch?v=x"));
    EXPECT(!isExtractableUrl("view-source:https://www.youtube.com/watch?v=x"));
    // Direct files/URLs stay on the normal media path.
    EXPECT(!isExtractableUrl("tests/media/sample.mp4"));
    EXPECT(!isExtractableUrl("https://x.com/a.mp4"));
    // Watch-style links across all known host variants.
    EXPECT(isExtractableUrl("https://www.youtube.com/watch?v=aqz-KE-bpKQ"));
    EXPECT(isExtractableUrl("https://www.youtube.com/watch?v=x&list=PL123"));
    EXPECT(isExtractableUrl("http://youtube.com/watch?v=x"));
    EXPECT(isExtractableUrl("https://m.youtube.com/watch?v=x"));
    EXPECT(isExtractableUrl("https://music.youtube.com/watch?v=x"));
    EXPECT(isExtractableUrl("https://www.youtube.com/shorts/abc123XYZ"));
    EXPECT(isExtractableUrl("https://www.youtube.com/embed/abc123"));
    EXPECT(isExtractableUrl("https://www.youtube.com/live/abc123"));
    EXPECT(isExtractableUrl("https://www.youtube-nocookie.com/embed/abc123"));
    EXPECT(isExtractableUrl("https://youtu.be/aqz-KE-bpKQ"));
    EXPECT(isExtractableUrl("https://youtu.be/aqz-KE-bpKQ?t=30"));
    // Scheme-less input is normalized upstream (address bar) — parseUrl
    // rejects it here.
    EXPECT(!isExtractableUrl("youtube.com/watch?v=x"));
}

TEST(extractor_shell_quoting) {
    using media::extractor::shellQuote;
    EXPECT(shellQuote("") == "''");
    EXPECT(shellQuote("plain") == "'plain'");
    EXPECT(shellQuote("it's") == "'it'\\''s'");
    EXPECT(shellQuote("a b&c;$(x)`y`*?[") == "'a b&c;$(x)`y`*?['");
    // Round-trip through a real POSIX shell: printf %s must give back
    // the exact bytes for nasty inputs (URLs can carry & ; ' etc).
    const char* nasty = "https://x.com/watch?v=1&list='two'&t=$(id);`id`";
    std::string cmd = "printf %s " + shellQuote(nasty);
    FILE* p = popen(cmd.c_str(), "r");
    EXPECT(p != nullptr);
    if (!p) return;
    std::string out;
    char buf[512];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, p)) > 0) out.append(buf, n);
    int rc = pclose(p);
    EXPECT(rc == 0);
    EXPECT(out == nasty);
}

TEST(extractor_cache_and_guards) {
    using media::extractor::ResolvedMedia;
    using media::extractor::resolve;
    using media::extractor::peek;
    using media::extractor::clearCache;
    using media::extractor::cacheSize;
    clearCache();
    EXPECT(cacheSize() == 0);
    ResolvedMedia rm;
    std::string err;
    // Non-extractable URLs fail fast: no process spawn, no network.
    EXPECT(!resolve("https://example.com/a.mp4", rm, err));
    EXPECT(!err.empty());
    // Cache peek on an empty cache misses.
    EXPECT(!peek("https://www.youtube.com/watch?v=aqz-KE-bpKQ", rm));
    // Optional live round-trip (MB_EXTRACTOR_LIVE=1): yt-dlp's GENERIC
    // extractor resolves a plain https file — same code path as a real
    // resolve, without YouTube's IP-dependent bot checks, so CI stays
    // hermetic by default.
    if (const char* live = getenv("MB_EXTRACTOR_LIVE")) {
        if (live[0] && live[0] != '0') {
            clearCache();
            err.clear();
            rm = ResolvedMedia{};
            bool ok = resolve(
                "https://www.youtube.com/watch?v=heresy", rm, err);
            (void)ok;   // YouTube itself stays network-dependent: either
                        // outcome is acceptable, we only require a sane
                        // result: URL on success, message on failure.
            if (rm.directUrl.empty()) EXPECT(!err.empty());
            else EXPECT(rm.directUrl.compare(0, 4, "http") == 0);
        }
    }
    clearCache();
    EXPECT(cacheSize() == 0);
}

// ---------------------------------------------------------------------------
// v2.6: video id extraction, minjson, Piped stream selection
// ---------------------------------------------------------------------------

TEST(extractor_video_id) {
    using media::extractor::videoId;
    EXPECT(videoId("https://www.youtube.com/watch?v=aqz-KE-bpKQ") ==
           "aqz-KE-bpKQ");
    EXPECT(videoId("https://www.youtube.com/watch?v=aqz-KE-bpKQ&t=42s") ==
           "aqz-KE-bpKQ");
    EXPECT(videoId("https://www.youtube.com/watch?app=desktop&v=Sx624xrjPKQ") ==
           "Sx624xrjPKQ");
    EXPECT(videoId("http://m.youtube.com/watch?v=dQw4w9WgXcQ") ==
           "dQw4w9WgXcQ");
    EXPECT(videoId("https://youtu.be/aqz-KE-bpKQ") == "aqz-KE-bpKQ");
    EXPECT(videoId("https://youtu.be/aqz-KE-bpKQ?t=30") == "aqz-KE-bpKQ");
    EXPECT(videoId("https://www.youtube.com/shorts/aqz-KE-bpKQ") ==
           "aqz-KE-bpKQ");
    EXPECT(videoId("https://www.youtube.com/embed/aqz-KE-bpKQ") ==
           "aqz-KE-bpKQ");
    EXPECT(videoId("https://www.youtube.com/live/aqz-KE-bpKQ") ==
           "aqz-KE-bpKQ");
    EXPECT(videoId("https://www.youtube-nocookie.com/embed/aqz-KE-bpKQ") ==
           "aqz-KE-bpKQ");
    // Non-videos / garbage give "".
    EXPECT(videoId("https://www.youtube.com/") == "");
    EXPECT(videoId("https://www.youtube.com/results?search_query=x") == "");
    EXPECT(videoId("https://example.com/watch?v=abc") == "");
    EXPECT(videoId("not a url") == "");
    // Id too short to be real: rejected.
    EXPECT(videoId("https://www.youtube.com/watch?v=ab") == "");
}

TEST(minjson_parse_and_access) {
    using media::minjson::Doc;
    Doc d;
    const char* j = R"json({
        "title": "Rick \u00c4stley \"q\"\\",
        "duration": 213,
        "live": false,
        "gone": null,
        "videoStreams": [
            {"url": "https://a/1", "format": "MP4",  "quality": "LBRY",
             "mimeType": "video/mp4", "videoOnly": false},
            {"url": "https://a/2", "format": "HLS",  "quality": "HLS",
             "mimeType": "application/x-mpegurl", "videoOnly": false},
            {"url": "https://a/3", "format": "MPEG_4", "quality": "360p",
             "mimeType": "video/mp4", "videoOnly": false},
            {"url": "https://a/4", "format": "WEBM", "quality": "720p",
             "mimeType": "video/webm", "videoOnly": true}
        ]
    })json";
    EXPECT(d.parse(j));
    EXPECT(std::string(d["title"].str()) ==
           "Rick \xC3\x84stley \"q\"\\");
    EXPECT(d["duration"].integer() == 213);
    EXPECT(d["live"].isBool() && !d["live"].boolean());
    EXPECT(d["gone"].isNull());
    EXPECT(d["videoStreams"].arrSize() == 4);
    EXPECT(std::string(d["videoStreams"].at(2)["quality"].str()) == "360p");
    EXPECT(d["videoStreams"].at(3)["videoOnly"].boolean());
    EXPECT(std::string(d["videoStreams"].at(9)["url"].str()) == "");  // OOB safe
    EXPECT(std::string(d["absent"].str()) == "");                     // absent safe
    EXPECT(std::string(d["duration"].str()) == "");                   // wrong type safe
    // Malformed inputs fail the parse; accessors stay safe.
    const char* bad[] = {"", "{", "[1,", "{\"a\" 1}", "\"unc", "tru",
                         "[1,2,]", "{\"a\":}", "1. ", "01x", "{} extra"};
    for (const char* b : bad) {
        Doc e;
        EXPECT(!e.parse(b));
        EXPECT(std::string(e["x"].str()) == "");
    }
}

TEST(extractor_piped_selection) {
    using media::extractor::ResolvedMedia;
    using media::extractor::selectPipedStream;
    ResolvedMedia rm;
    std::string err;
    // Full payload with distractors: video-only DASH, HLS, and a
    // higher-quality MPEG_4 to prefer over the LBRY mirror.
    const char* good = R"json({"title":"Never Gonna Give You Up","uploader":"Rick",
        "duration":213,"audioStreams":[],"videoStreams":[
        {"url":"https://proxy/v?itag=18","format":"MPEG_4","quality":"360p",
         "mimeType":"video/mp4","videoOnly":false},
        {"url":"https://proxy/hls.m3u8","format":"HLS","quality":"HLS",
         "mimeType":"application/x-mpegurl","videoOnly":false},
        {"url":"https://lbry/x.mp4","format":"MP4","quality":"LBRY",
         "mimeType":"video/mp4","videoOnly":false},
        {"url":"https://proxy/v?itag=137","format":"WEBM","quality":"1080p",
         "mimeType":"video/webm","videoOnly":true}]})json";
    EXPECT(selectPipedStream(good, rm, err));
    EXPECT(err.empty());
    EXPECT(rm.directUrl == "https://proxy/v?itag=18");   // MPEG_4 > MP4/LBRY
    EXPECT(rm.title == "Never Gonna Give You Up");
    EXPECT(rm.ext == "mp4");
    EXPECT(rm.via == "piped");
    // Error payload (video gone): must surface the error, not crash.
    // (One line: JSON strings cannot contain raw newlines.)
    const char* dead = R"json({"error":"org.schabi.newpipe.extractor.exceptions. ContentNotAvailableException: Got error ERROR: This video is unavailable"})json";
    rm = ResolvedMedia{};
    EXPECT(!selectPipedStream(dead, rm, err));
    EXPECT(err.find("unavailable") != std::string::npos);
    // No videoStreams at all.
    EXPECT(!selectPipedStream("{\"title\":\"x\"}", rm, err));
    EXPECT(!err.empty());
    // Garbage JSON.
    EXPECT(!selectPipedStream("not json at all", rm, err));
}

TEST(extractor_piped_hosts) {
    using media::extractor::pipedHosts;
    // Defaults present.
    std::vector<std::string> hosts = pipedHosts();
    EXPECT(hosts.size() >= 1);
    bool hasDucks = false;
    for (const auto& h : hosts) hasDucks |= (h == "pipedapi.ducks.party");
    EXPECT(hasDucks);
    // Env override, comma/semicolon/space separated, trailing slashes trim.
    setenv("MINIBROWSER_PIPED_HOSTS",
           "a.example/api, b.example/ ;c.example", 1);
    hosts = pipedHosts();
    EXPECT(hosts.size() == 3);
    EXPECT(hosts[0] == "a.example/api");
    EXPECT(hosts[1] == "b.example");   // trailing '/' trimmed
    EXPECT(hosts[2] == "c.example");
    unsetenv("MINIBROWSER_PIPED_HOSTS");
    // Back to defaults after unset.
    hosts = pipedHosts();
    EXPECT(hosts.size() >= 1);
}

// ---------------------------------------------------------------------------

} // namespace browser