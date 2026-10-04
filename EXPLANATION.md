# What was wrong and what I changed

This document walks through every bug I found in your `browser.zip` and
explains each fix. I've grouped them into four buckets:

1. **Build / project-structure bugs** — these prevented the project from
   compiling at all.
2. **Crash / leak bugs** — undefined behavior or resource leaks.
3. **Logic / correctness bugs** — code compiled and ran but produced the
   wrong output.
4. **Performance bugs** — code worked but was needlessly slow.
5. **Missing features** — things a basic HTML browser needs that were
   absent (some added, some noted as TODO).

---

## 1. Build / project-structure bugs

### 1.1 The directory layout didn't match the `#include` paths

Your `main.cpp` started with:

```cpp
#include "css/style.h"
#include "html/parser.h"
#include "js/jsengine.h"
#include "layout/font_loader.h"
#include "layout/layout.h"
#include "render/renderer.h"
#include "render/window.h"
#include "./tests/selftest.h"
```

…and `layout.h` had `#include "../css/style.h"`, `renderer.h` had
`#include "../layout/layout.h"`, etc. Those `../` and `subdir/` paths only
work if the files are physically arranged like:

```
browser/
├── main.cpp
├── css/       style.{h,cpp}
├── html/      lexer.{h,cpp}, parser.{h,cpp}
├── js/        jsengine.{h,cpp}
├── layout/    font_loader.{h,cpp}, layout.{h,cpp}, resource.{h,cpp}
├── render/    renderer.{h,cpp}, window.{h,cpp}
└── tests/     selftest.{h,cpp}
```

But every file you zipped was sitting in **one flat directory**. So none
of the `#include "../…"` or `#include "css/…"` lines could resolve, and
the project wouldn't even start compiling.

**Fix:** I moved each file into the subdirectory its `#include` paths
already assumed. (I also fixed `#include "./tests/selftest.h"` to
`#include "tests/selftest.h"` for consistency.)

### 1.2 There was no build system

You'd have to manually type a long `g++` line with all 11 source files.
**Fix:** I added a `Makefile` that auto-discovers every `.cpp` under the
tree, links `sdl2 / SDL2_ttf / SDL2_image` via `pkg-config`, and exposes
three targets:

```bash
make            # build the browser
make selftest   # run the in-process unit tests (no SDL window needed)
make run        # run the browser against test.html
```

### 1.3 There was no sample HTML to actually display

`main.cpp` defaults to `test.html` if no argument is given, but you
shipped no `test.html`. The browser would just print "Could not open
test.html" and exit.

**Fix:** I added a `test.html` that exercises headings, paragraphs,
lists, inline links, images, CSS classes, borders, `<pre>`, block
comments in JS, and an onclick handler that mutates the DOM. So when
you `make run` you actually see something.

---

## 2. Crash / leak bugs

### 2.1 `SDL_Init` / `TTF_Init` / `createWindow` failures were swallowed

`main.cpp` had this pattern four times:

```cpp
if (SDL_Init(SDL_INIT_VIDEO) != 0) { /* ... */ }
```

The `/* ... */` was literally empty. If `SDL_Init` failed (no display,
no permissions, etc.), the program kept going into `TTF_Init`,
`loadFont`, `createWindow`, `SDL_CreateRenderer`, and `layout` — most of
which would dereference null pointers and segfault. The crash site
would be far from the actual cause, so debugging would be very hard.

**Fix:** I added an `sdlDie(where)` helper that prints `SDL_GetError()`,
calls `SDL_Quit()`, and `std::exit(1)`. Every `if (!= 0)` / `if (!ptr)`
check in startup now calls `sdlDie`, so a startup failure prints a
clear message and exits cleanly.

### 2.2 The SDL cursor was leaked on every mouse move

In `SDL_MOUSEMOTION` you had:

```cpp
SDL_SetCursor(SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW));
// or
SDL_SetCursor(SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_HAND));
```

`SDL_CreateSystemCursor` allocates a new `SDL_Cursor` every call, and
`SDL_SetCursor` does **not** free the previous one. So every time the
mouse moved between a link and non-link area, you allocated another
cursor that was never freed. On a 60 FPS event loop, this leaks fast.

**Fix:** Create both cursors **once** at startup, store them in
`cursorArrow` / `cursorHand`, and only switch between them with
`SDL_SetCursor`. Track which one is currently active (`usingHandCursor`)
so we don't call `SDL_SetCursor` on every motion event. At shutdown,
call `SDL_FreeCursor` on both.

### 2.3 The relayout dropped the `baseDir` argument

`main.cpp` correctly passed `baseDir` on the first layout:

```cpp
LayoutResult lr = layout(dom, WIN_W, font, cssRules, baseDir);
```

…but the relayout after a JS mutation didn't:

```cpp
if (needsRelayout) {
    lr = layout(dom, WIN_W, font, cssRules);   // ← missing baseDir
    ...
}
```

Inside `layout()` the first thing `ResourceLoader::instance().setBaseDir(baseDir)`
is called. So after a relayout with the default `baseDir = ""`, every
relative `<img src="cat.png">` would resolve against the current
working directory instead of the HTML file's directory. Since images
loaded fine on first paint and then vanished after any JS-driven
relayout, this was very confusing to debug.

**Fix:** Pass `baseDir` on the relayout call too. Your own comment on
line 75 even said "pass baseDir on relayout" — the fix is just making
the call match the comment.

### 2.4 `SDL_DestroyRenderer` was called before the image texture cache was freed

I added an image texture cache in the renderer (see 4.1 below). Those
textures are tied to the `SDL_Renderer`, so they must be freed **before**
`SDL_DestroyRenderer`. I added `clearImageTextureCache()` and called
it in the right order at shutdown.

---

## 3. Logic / correctness bugs

### 3.1 The hover color was applied to ALL underlined text, not just the hovered link

In `renderer.cpp`:

```cpp
SDL_Color drawColor = run.style.color;
if (!hoveredHref.empty() && run.style.underline) {
    drawColor = hoverColor;
}
```

This colored **every** underlined run red whenever **any** link was
hovered. Two consequences:

- A non-link `<u>underlined</u>` would turn red whenever you hovered
  over some link elsewhere on the page.
- If two links had different hrefs, hovering one turned both red.

**Fix:** Only color the run if it's actually a link AND its href
matches the hovered one:

```cpp
if (run.isLink && !hoveredHref.empty() && run.href == hoveredHref) {
    drawColor = hoverColor;
}
```

### 3.2 `<a>` was rendered as a block element

In `layout.cpp`, the list of "block-level" tags included `a`:

```cpp
if (node->tag == "p" || node->tag == "h1" || ... || node->tag == "a" || ...) {
```

`<a>` is **inline** in HTML. The block branch creates a new `Box` for
each `<a>`, forcing it onto its own line. So a paragraph like:

```html
<p>Click <a href="x">here</a> to continue.</p>
```

…was rendered as three lines:
```
Click
here
to continue.
```

instead of one line. The `collectRuns` function already handles `<a>`
correctly as inline — it just never got the chance because the block
branch grabbed the `<a>` first.

**Fix:** Remove `"a"` from the block-level list. Now `<a>` is handled
by `collectRuns` when its parent block calls it, and "Click here to
continue." renders on one line with the link inline.

### 3.3 Borders were not drawn on background-only boxes

`renderer.cpp` had:

```cpp
for (auto& b : boxes) {
    if (!b.isImage && !b.lines.empty()) {
        drawBorder(ren, b, scrollY);
    }
}
```

A `<div style="background: red; border: 2px solid black"></div>` has no
text, so `b.lines.empty()` is true, so the border wasn't drawn. The bg
paints, but the border is silently skipped.

**Fix:** Drop the `!b.lines.empty()` check; draw the border on any
non-image box whose border width is non-zero:

```cpp
for (auto& b : boxes) {
    if (b.isImage) continue;
    const Edges& bd = b.style.border;
    if (bd.vertical() == 0 && bd.horizontal() == 0) continue;
    drawBorder(ren, b, scrollY);
}
```

### 3.4 The parser destroyed the tree on a stray close tag

`parser.cpp`'s CLOSE handler was:

```cpp
case TokKind::CLOSE: {
    flushText();
    while (stack.size() > 1) {
        auto top = stack.back();
        stack.pop_back();
        if (top->tag == tk.name) break;
    }
    break;
}
```

If the document had a stray `</div>` with no matching `<div>`, this loop
popped **everything** off the stack down to the root, so every
subsequent tag in the document became a sibling of the root instead of
being nested. The rest of the page would render completely flat.

**Fix:** Walk the stack from the top looking for a matching ancestor.
If found, pop down to and including it. If not found, the close tag is
stray — ignore it:

```cpp
size_t match = std::string::npos;
for (size_t k = stack.size(); k > 1; --k) {
    if (stack[k - 1]->tag == tk.name) { match = k - 1; break; }
}
if (match == std::string::npos) break;   // stray
stack.resize(match);
```

### 3.5 The JS engine skipped multi-line block comments wrong

`jsengine.cpp`'s `execute()` did:

```cpp
while (std::getline(iss, line)) {
    std::string t = trim(line);
    if (startsWith(t, "/*")) continue;   // ← only skips the START line
    ...
}
```

For a multi-line block comment like:

```js
/*
 * This is a comment
 */
```

…the first line is skipped, but the next two lines (`* This is a
comment` and `*/`) get parsed as statements and produce spurious "not
implemented" warnings.

**Fix:** Track `/* … */` block state across statements. For each
statement fragment, scan character-by-character: skip everything inside
an open block comment, look for `*/` to close it, and also handle
`//` line comments inline.

### 3.6 The JS engine couldn't handle two statements on one line

`execute()` used `std::getline` to read one line at a time. So a line
like:

```js
var a = 1; var b = 2;
```

…was passed to `handleVarDecl` as a single statement. The parser
correctly found the first `=` (after `a`) and treated the RHS as
`1; var b = 2`, which `std::stod` parsed as `1` (it stops at the first
non-digit). The `b = 2` half was silently dropped.

**Fix:** Use the existing `splitStatements()` helper to split the whole
script on top-level `;` (respecting strings and bracket depth) before
processing. Each resulting fragment is then one logical statement.

### 3.7 The lexer treated every `<` as a tag start

`lexer.cpp` saw a `<` and immediately looked for the matching `>`. So
text like `a < b` was treated as a tag named `b`, and either the rest
of the document was lost (no `>`) or `a < b > c` was mis-parsed as a
tag. HTML's actual rule is: a `<` followed by a letter, `/`, or `!`
starts a tag; otherwise it's literal text.

**Fix:** After pulling `inside = html.substr(i+1, j-i-1)`, check the
first character. If it's not a letter, `/`, or `!`, emit a single `<`
as TEXT and continue from `i+1`.

### 3.8 Unknown containers dropped their text

`layout.cpp` had a block branch for known tags (`p`, `h1`–`h3`, `div`,
`li`, `button`, `pre`) and a generic-container branch for everything
else. The generic branch only laid out child elements — any text
directly inside an unknown container was silently dropped. So:

```html
<article>Hello world</article>
```

…rendered as nothing. Same for `<section>`, `<header>`, `<footer>`,
`<main>`, `<nav>`.

**Fix:** In the generic container branch, detect when the container
has direct inline children (text, `<a>`, `<b>`, `<i>`, `<span>`,
`<code>`, `<br>`). If so, run `collectRuns` on it and emit a single
synthesized `Box`, exactly like the block branch does for `<p>`.

### 3.9 The link click handler walked the whole DOM

`main.cpp`'s click handler had a recursive `findA` lambda that walked
the entire DOM tree looking for the first `<a>` whose `href` matched
the clicked link's. So:

- It was O(N) per click.
- If two `<a>`s shared the same href, only the first one's `onclick`
  ran, even if you clicked the second.
- The `<a>` was found by href only, not by position — clicking the
  visual link could fire the wrong onclick if multiple `<a>`s had the
  same href.

**Fix:** The layout now stores a `weak_ptr<Node>` to the actual `<a>`
on each `Link` hit-rect (via `Run.linkNode` set in `collectRuns`).
The click handler locks the weak pointer to get the `<a>` directly —
O(1) and always the right one. The `findA` walk only runs as a
fallback if the weak pointer has expired (e.g., the DOM was mutated
between layout and click).

---

## 4. Performance bugs

### 4.1 Image textures were recreated on every frame

`renderer.cpp` did this for each image, every frame:

```cpp
SDL_Texture* tex = SDL_CreateTextureFromSurface(ren, img->surface);
SDL_RenderCopy(ren, tex, nullptr, &dst);
SDL_DestroyTexture(tex);
```

`SDL_CreateTextureFromSurface` is an expensive GPU upload. Doing it
for every image on every frame (60 FPS) is the slowest possible path.
For a page with 10 images at 60 FPS, that's 600 texture uploads per
second, all of which produce identical textures.

**Fix:** Added a `std::unordered_map<std::string, SDL_Texture*>` cache
in `renderer.cpp` keyed by image URL. On first encounter we create
the texture and store it; subsequent frames reuse it. The cache is
cleared by `clearImageTextureCache()` at shutdown (which must happen
**before** `SDL_DestroyRenderer`, since textures are tied to the
renderer).

### 4.2 Duplicate first-frame render

`main.cpp` had `render(ren, font, lr.boxes, 0, "");` on line 99,
*before* the event loop, and then the loop's first iteration called
`render` again. The pre-loop render was wasted work.

**Fix:** Removed the redundant pre-loop call.

---

## 5. Missing features (not all fixed; noted as TODOs)

These aren't bugs in the strict sense, but they're gaps a real
"displays actual HTML files" browser needs:

- **External `<script src="app.js">`** — `extractJS` only reads inline
  script bodies. External scripts aren't fetched. Add a file loader
  in `extractJS` that reads `src` attributes against `baseDir`.
- **Navigation** — clicking a link prints `[Link clicked]` but
  doesn't load the target page. Add a `loadPage(filename)` function
  that re-runs parse → style → layout, and call it on link click.
- **Text rendering performance** — `TTF_RenderUTF8_Blended` is still
  called per run per frame. Cache glyphs or cache rendered surfaces
  keyed by `(text, font, color)`.
- **Line wrapping of long words** — a single word wider than the
  content box overflows instead of breaking. Add a fallback that
  breaks long words at the box edge.
- **Inline `<span>` with style** — `<span style="color: red">` should
  color its text. Currently `collectRuns` doesn't compute style for
  `<span>`. Easy fix: add `span` to the same handling path as `<b>`
  / `<i>`.
- **Lists** — `<ul>` / `<ol>` are not styled; no bullet / number
  rendering.
- **`<img>` from remote URLs** — silently fails. Could shell out to
  `curl` or add a minimal HTTP client.
- **Tables, forms, inputs** — not supported.
- **CSS cascading** — only tag, `.class`, `#id` selectors, no
  descendant selectors like `nav a`, no specificity calculation.

---

## Summary

| # | File                     | Bug                                                         | Severity |
|---|--------------------------|-------------------------------------------------------------|----------|
| 1.1 | all headers             | Flat layout didn't match `#include "subdir/..."` paths     | **Blocker** |
| 1.2 | (none)                  | No build system                                             | High     |
| 1.3 | (none)                  | No sample `test.html`                                       | High     |
| 2.1 | main.cpp                | Empty `/* ... */` swallowed SDL errors                      | High     |
| 2.2 | main.cpp                | Cursor leaked on every mouse move                            | Medium   |
| 2.3 | main.cpp                | `layout()` relayout dropped `baseDir`                       | **Blocker** for images |
| 2.4 | main.cpp                | Renderer destroyed before texture cache freed               | Medium   |
| 3.1 | renderer.cpp            | Hover color applied to all underlined text                  | Medium   |
| 3.2 | layout.cpp              | `<a>` treated as block, breaking inline flow                | High     |
| 3.3 | renderer.cpp            | Borders skipped on background-only boxes                   | Medium   |
| 3.4 | parser.cpp              | Stray close tag collapsed the tree                          | High     |
| 3.5 | jsengine.cpp            | Multi-line `/* */` not skipped                              | Low      |
| 3.6 | jsengine.cpp            | Two statements per line dropped the second                 | Medium   |
| 3.7 | lexer.cpp               | `a < b` parsed as a tag                                      | Medium   |
| 3.8 | layout.cpp              | Unknown containers dropped direct text                       | Medium   |
| 3.9 | main.cpp                | Link click handler walked the DOM                           | Low/Med  |
| 4.1 | renderer.cpp            | Image texture recreated every frame                          | Medium   |
| 4.2 | main.cpp                | Duplicate first-frame render                                | Low      |
| 5.1 | tests/selftest.cpp      | `css_selectors_basic` expected `p.foo#bar` to match an element with only `class=foo` (no `id=bar`) | Low (test bug) |

The "Blocker" rows are the ones that completely broke the project —
no build (1.1) and silently-lost images after any JS mutation (2.3).
After fixing those plus the high-severity items, the browser compiles,
runs `test.html`, and renders headings / paragraphs / lists / inline
links / CSS-styled cards with borders / `<pre>` blocks / inline JS
with `console.log` / and an onclick handler that mutates the DOM and
triggers a relayout that keeps the images working.

All 103 self-tests in `tests/selftest.cpp` now pass (was 102/103 before
fixing the `css_selectors_basic` assertion).

---

# Part 2 — Features added to make this an actual browser

The first pass fixed bugs. The user then asked to "add everything you
see fit to make it an actual browser". This section documents the new
features added on top of the bug-fixed codebase.

## Architecture: Browser class

All browser-level state (DOM, layout, history, scroll position, cursors,
navigation) used to live in `main.cpp` as scattered local variables. As
features piled up, that became unmaintainable. So I introduced a
`Browser` class:

- **`app/browser.h`** — declaration
- **`app/browser.cpp`** — implementation
- **`main.cpp`** — now a thin ~50-line entry point that creates a
  `Browser`, runs the SDL event loop, calls `browser.handleEvent()` per
  event and `browser.paint()` per frame.

This makes the high-level orchestration easy to read and easy to
extend. The `Browser` class owns:

```
SDL_Window*      win_;
SDL_Renderer*   ren_;
TTF_Font*       font_;
int             winW_, winH_;     // current window size

std::string     currentPath_;    // absolute path of loaded page
std::string     baseDir_;        // directory of currentPath_
NodePtr         dom_;            // parsed DOM
CSSRule[]       cssRules_;       // parsed CSS (external + inline)
LayoutResult    layout_;         // current layout
JSEngine        js_;             // JS interpreter
int             scrollY_;
std::string     hoveredHref_;
bool            needsRelayout_;

history_                      // back/forward stack
historyIndex_
```

## New feature: navigation

`Browser::navigate(path)` resolves `path` against the current page's
directory (or uses it as-is if absolute), reads the HTML, parses it,
loads external CSS and JS, runs the script, lays out the page, and
resets the scroll. Clicking a link in the page calls `navigate()`
instead of just printing `[Link clicked]` — so links actually load the
target page.

The `onclick` handler still runs *before* navigation, so JS-side
interception (e.g., recording analytics, mutating state) works.
Navigation skips remote URLs (`http://...`) and `javascript:` / `#`
hrefs — the previous two would have crashed or no-op'd.

## New feature: back / forward / reload

- **`Browser::back()`** — pop one entry off the history stack
- **`Browser::forward()`** — push one entry back on
- **`Browser::reload()`** — re-read the current file from disk

Keyboard shortcuts:

- **Alt+Left** → back
- **Alt+Right** → forward
- **F5** or **Ctrl+R** → reload

Plus on-screen buttons in the address bar (`<`, `>`, `R`) for the
mouse users. Disabled buttons are drawn grayed out and don't respond
to clicks.

## New feature: address bar UI

A 32px-tall bar across the top of the window containing:

1. Three square buttons (`<`, `>`, `R`) for back / forward / reload
2. A text field showing the absolute path of the current page

The field is just a display — there's no editable input (typing
characters doesn't navigate yet; you'd need text input focus, see
"Future work" below).

## New feature: status bar UI

A 22px-tall bar at the bottom of the window that shows the URL of
the currently hovered link, or the current page path when no link
is hovered. Standard browser behavior.

## New feature: scrollbar

A 12px-wide vertical scrollbar on the right edge of the content
area, with a track and a thumb sized proportionally to the
visible/content ratio. The thumb position reflects the current
`scrollY`. Drag-to-scroll is not yet implemented (TODO).

## New feature: external CSS

`<link rel="stylesheet" href="style.css">` is now honored. The
`Browser::loadPage_` function walks the DOM for `<link>` tags whose
`rel` attribute contains "stylesheet", resolves their `href` against
the page's directory, reads the file, and prepends its content to
the CSS string before parsing. So `<link>` and inline `<style>` both
contribute to the parsed rules.

## New feature: external JS

`<script src="app.js"></script>` is now honored. Same walk pattern:
find every `<script src>` tag, read its file, prepend the content
to the JS string before executing. External scripts run **first**
(so library globals are defined before the inline script that uses
them), matching browser behavior.

## New feature: lists with markers

`<ul>` and `<ol>` are now first-class block containers. The layout
indents their `<li>` children by 20px and prepends a marker run to
each `<li>`'s runs:

- `<ul>` → `•` (U+2022 BULLET, two spaces)
- `<ol>` → `1. `, `2. `, `3. `, … (counting preceding `<li>` siblings)

So ordered lists now read:

```
1. First item
2. Second item with a link inside
3. Third item
```

…instead of three unmarked paragraphs.

## New feature: tables

Basic table support:

- `<table>` is now a block-level container
- Walks its `<tr>` children (and `<thead>`/`<tbody>`/`<tfoot>` wrappers)
- Computes column widths as the max content width of any cell in that
  column, shrinking if the total exceeds the page width
- Lays out each `<td>`/`<th>` as its own block with the computed
  column width
- `<th>` is automatically bolded
- Borders on `td` and `th` work via the existing `border` CSS

No cell-merging (`colspan`/`rowspan`), no table-specific CSS like
`border-collapse`. That's future work.

## New feature: compound CSS selectors

The old `matchesSelector(sel, tag, attrs)` only handled a single
simple selector (one tag/class/id combination). It couldn't evaluate
`nav a`, `div > p`, or `.card p`.

I added a new `matchesSelectorNode(sel, node)` that:

1. Tokenizes the selector into simple-selector / combinator pieces:
   - descendant (whitespace)
   - child (`>`)
   - adjacent sibling (`+`)
2. Evaluates recursively from the right: the rightmost simple
   selector must match the node, then walks the parent/sibling chain
   to evaluate the combinators to its left.

The layout's `computeStyle` now calls `matchesSelectorNode` instead
of the old simple matcher. The old `matchesSelector` stays as a
back-compat function for tests and for `matchSimple` (the simple
selector leaf evaluator).

## New feature: long-word breaking

If a single word is wider than the content box (e.g., a 200-character
unbreakable token, or a very long URL), the layout used to overflow
the right edge. Now, if a word's measured width exceeds `mw - 8`, the
layout walks it one codepoint at a time, building the longest prefix
that fits, then emits that prefix as its own run, breaks the line,
and continues with the rest of the word. UTF-8 decoding is done
correctly so multi-byte characters aren't split mid-sequence.

## New feature: window resize

The previous window was a fixed 800x600; resizing the OS window had
no effect on the layout. The `Browser` class now listens for
`SDL_WINDOWEVENT_RESIZED` / `SDL_WINDOWEVENT_SIZE_CHANGED`, updates
its cached `winW_` / `winH_`, and recomputes the layout. The address
bar / status bar / scrollbar all reposition to the new size, and
the scroll is clamped to the new maximum.

## Summary of new features

| Feature | Files touched |
|---|---|
| Browser class | new `app/browser.h`, `app/browser.cpp`; rewrite `main.cpp` |
| Navigation (click link loads page) | `app/browser.cpp` |
| Back/forward/reload + keyboard shortcuts | `app/browser.cpp` |
| Address bar UI | `app/browser.cpp` |
| Status bar UI | `app/browser.cpp` |
| Scrollbar | `app/browser.cpp` |
| External CSS (`<link>`) | `app/browser.cpp` |
| External JS (`<script src>`) | `app/browser.cpp` |
| Lists (`<ul>` bullets, `<ol>` numbers) | `layout/layout.cpp` |
| Tables (`<table>`, `<tr>`, `<td>`, `<th>`) | `layout/layout.cpp` |
| Compound selectors (`a b`, `a > b`, `a + b`) | `css/style.{h,cpp}`, `layout/layout.cpp` |
| Long-word breaking | `layout/layout.cpp` |
| Window resize triggers relayout | `app/browser.cpp` |

## Demo files

- `test.html` — the home page; exercises everything
- `page2.html` — the navigation target
- `style.css` — external stylesheet loaded by both pages
- `app.js` — external JS file loaded by test.html

`make run` opens test.html. Click the "About" link in the top `<nav>`
to navigate to page2.html. Press Alt+Left to come back. Press F5 to
reload. Hover a link to see its URL in the status bar.

## Tests added

4 new unit tests for the new logic:
- `css_compound_descendant` — `nav a` matches a link inside `<nav>`
- `css_compound_child` — `div > p` matches a direct child only
- `css_compound_class_descendant` — `.card p` matches `<p>` inside `.card`
- `list_ol_marker` — `<ol>` parses correctly with `<li>` children

Total self-tests: 116 (up from 103 in the bug-fix pass), all passing.

## Future work (not done)

- **Text input fields** (`<input type="text">`, `<textarea>`): would
  require keyboard focus management, cursor blinking, text editing
  state — significantly more work.
- **Editable address bar**: same as above.
- **Click-to-drag scrollbar thumb**: would let you click and drag
  the scrollbar to scroll.
- **`colspan` / `rowspan`** in tables.
- **CSS specificity / cascade order**: rules are applied in source
  order, no specificity calculation. So `nav a` and `a` both apply
  in source order; the latest wins. Real browsers compute
  specificity (ID > class > tag) and resolve ties by source order.
- **Forms / `<form>` submission**: would need text inputs first.
- **`<canvas>`, `<svg>`, `<video>`**: not implemented.
- **Remote image loading** (`<img src="https://...">`): would need
  an HTTP client (curl or similar).
- **JS event listeners** (`addEventListener`, `removeListener`):
  currently only inline `onclick` attributes work.
- **DOM mutation via JS** beyond `textContent` / `style`: no
  `appendChild`, `createElement`, `removeChild`, etc.

---

# Part 3 — Round 2: more features + click/hover bug fixes

The user reported "some links and buttons don't click". Two bugs were
found and fixed, plus seven new features were added on top of round 1.

## Critical bug fixes (the "links don't click" issue)

### 3.1 Hover cursor was off by `ADDRESS_BAR_H` pixels

`Browser::updateHoverCursor()` called `linkAt(mouseX_, mouseY_)` with
**window coordinates**. But `linkAt` expected **content-relative**
coordinates (it adds `scrollY_` to convert to document coords). The
layout's link hit-rects are in document coords, and they sit below the
address bar (y ≥ ADDRESS_BAR_H = 32 in window space, or y ≥ 0 in content
space). Passing window coords meant every link appeared to be 32px lower
than it actually was — so when you hovered over a link visually, the
hit-test thought you were hovering 32px below it (usually empty space).

**Symptom**: The hand cursor never appeared over links, even though
clicking them worked (because `handleClick` did the offset correction
correctly). Users thought the link was dead.

**Fix**: Subtract `contentY()` before calling `linkAt`, and only call it
when the mouse is actually in the content area:

```cpp
if (mouseY_ >= contentY() && mouseY_ < contentY() + contentH()) {
    h = linkAt(mouseX_, mouseY_ - contentY());
}
```

### 3.2 Box click loop iterated forward, hitting the background first

When the page has a `<body style="background: ...">` (very common —
`style.css` sets `body { background: #fafafa }`), `layout.cpp` inserts
a body-background box at the **front** of the `boxes` vector (via
`boxes.insert(boxes.begin(), bg)`). Container `<div>`s with backgrounds
do the same via `boxes.insert(firstChildIdx, bg)`.

The `handleClick` box loop iterated **forward** (`for (auto& b : boxes)`),
so the bg box was visited **first**. The bg box has `sourceNode = body`
(or the div), `findAttr(body, "onclick")` returns "" (no onclick on
body), and the loop **broke** without ever visiting the actual content
box (the `<p>`, `<button>`, etc. with the onclick handler).

**Symptom**: Clicking on a `<div onclick>` or `<button onclick>` did
nothing. Links still worked because the link loop ran before the box
loop. Pages without a body/container background were unaffected, which
is why "sometimes" buttons didn't click.

**Fix**: Iterate in **reverse** so the topmost (last-painted) box is
visited first. Only break when an onclick actually fires, so a
transparent container falls through to the next box down:

```cpp
for (auto it = layout_.boxes.rbegin(); it != layout_.boxes.rend(); ++it) {
    const Box& b = *it;
    if (hit) {
        if (findAttr(hitNode, "onclick") fired) {
            js_.executeEvent(...);
            break;  // event fired, stop
        }
        // else: keep looking at boxes underneath
    }
}
```

## New feature: editable address bar

Click the URL field at the top of the window. It switches to editing
mode (blue border, blinking caret). Type a path (relative or absolute)
and press Enter to navigate. Esc cancels.

Implementation:
- New state: `addressEditing_`, `addressInput_`, `cursorBlinkTick_`
- On mouse-down inside the URL field rect, set `addressEditing_ = true`
  and call `SDL_StartTextInput()` so SDL sends `TEXTINPUT` events
- `TEXTINPUT` events append to `addressInput_`
- `KEYDOWN` handles `ESC` (cancel), `RETURN` (navigate), `BACKSPACE`
  (delete last char) when in editing mode
- `drawAddressBar_` renders the editing state with a blue border and
  caret
- Click outside the field exits editing mode without navigating

## New feature: scrollbar drag + click-on-track

Click and drag the scrollbar thumb to scroll. Click on the track
above or below the thumb to page up/down.

Implementation:
- `draggingScrollbar_`, `dragThumbOffset_` track the drag state
- `scrollbarThumbRect_()` computes the thumb's current rect
- On `MOUSEBUTTONDOWN` inside the scrollbar track:
  - If on the thumb: start a drag, remembering the y offset within
    the thumb so it doesn't snap to the cursor
  - If above/below the thumb: page up/down
- On `MOUSEMOTION` while dragging: recompute scrollY from the
  thumb's desired position
- On `MOUSEBUTTONUP`: stop dragging
- The thumb is highlighted (darker shade) when hovered or dragged

## New feature: find in page (Ctrl+F)

Press `Ctrl+F` to open the find bar (above the status bar). Type a
query; matches are highlighted in yellow, with the current match in
orange. Press Enter to go to the next match, Esc to close.

Implementation:
- `finding_`, `findQuery_`, `findMatches_` (vector of (boxIdx,
  lineIdx)), `findCurrent_` (index into findMatches_)
- `runFind_()`: walks every text run in every box; for each line that
  contains the query (case-insensitive), records (boxIdx, lineIdx)
- `findNext_()`: advances `findCurrent_` and scrolls the matching
  line into view
- New `renderWithHighlights()` in renderer.cpp: same as `render()` but
  draws a yellow/orange rect behind each highlighted line before
  painting the text on top
- The find bar shows the current match number (`3/12`) and a hint
  (`Enter=next  Esc=close`)

## New feature: page zoom (Ctrl+= / Ctrl+- / Ctrl+0)

Zoom in: `Ctrl+=` or `Ctrl+Plus`. Zoom out: `Ctrl+-`. Reset: `Ctrl+0`.
Range: 50% to 300% in 10% steps.

Implementation:
- `zoom_` (float), `baseCssRules_` (unzoomed snapshot)
- On `loadPage_`, store the parsed CSS in `baseCssRules_` and copy to
  `cssRules_`; then call `applyZoom_()`
- `applyZoom_()`: rebuilds `cssRules_` from `baseCssRules_` with all
  `font-size` values multiplied by `zoom_` (floored to 8px), then
  relayouts
- Tag-based default font sizes (h1=32, etc.) are NOT scaled (they're
  applied in `computeStyle` after CSS rules). This is a known
  limitation — would need to scale them too in `computeStyle`.

## New feature: `<hr>` horizontal rule

A 1px gray line spanning the content width, with 8px margins above
and below. Implemented as a background-only `Box` with `bg=gray`,
`h=1`, `w=width`.

## New feature: `<blockquote>` with left-border accent

Indents children 24px from the left and draws a 3px gray vertical bar
on the left edge spanning the blockquote's full height. The bar's
height is computed after laying out children, by tracking the child
start y and updating the bar box's `h` afterward.

## New feature: `<dl>` / `<dt>` / `<dd>` definition lists

- `<dl>` is a generic block container (just iterates children)
- `<dt>` is bolded and has a 6px top margin
- `<dd>` is indented 20px from the left

Both added to the block-level tags list in `layoutNode`.

## New feature: `display: none` (CSS)

A node with `display: none` is skipped entirely during layout — it and
its children don't render. Implemented at the top of `layoutNode`:
compute the style first, check `display`, return early if it's "none".

The `display` property is parsed in `applyStyle` (style.cpp) and
stored on `Style` as `display` / `hasDisplay`. The `computeStyle`
function copies it from matching CSS rules to the computed style.

## New feature: `text-align: center / right / justify`

The renderer now computes the total width of each line's runs and
shifts the starting x by:
- `(contentBoxW - lineWidth) / 2` for `center`
- `contentBoxW - lineWidth` for `right`

`text-align: left` (default) and `justify` are treated as left (we
don't implement word-spacing for justify).

## New feature: `<h4>` / `<h5>` / `<h6>` heading tags

Added to the block-level list and `computeStyle`:
- `<h4>`: 18px bold
- `<h5>`: 16px bold
- `<h6>`: 14px bold

## Summary of round 2 changes

| Type | What | Files |
|---|---|---|
| **Bug fix** | Hover cursor offset by ADDRESS_BAR_H | `app/browser.cpp` |
| **Bug fix** | Box click loop hit bg box first, broke before content | `app/browser.cpp` |
| Feature | Editable address bar | `app/browser.{h,cpp}` |
| Feature | Scrollbar drag + click-on-track | `app/browser.cpp` |
| Feature | Find in page (Ctrl+F) with highlight | `app/browser.{h,cpp}`, `render/renderer.{h,cpp}` |
| Feature | Page zoom (Ctrl+=/-/0) | `app/browser.{h,cpp}` |
| Feature | `<hr>` horizontal rule | `layout/layout.cpp` |
| Feature | `<blockquote>` with left border | `layout/layout.cpp` |
| Feature | `<dl>`/`<dt>`/`<dd>` definition lists | `layout/layout.cpp` |
| Feature | `display: none` | `css/style.{h,cpp}`, `layout/layout.cpp` |
| Feature | `text-align: center/right` | `css/style.{h,cpp}`, `render/renderer.cpp` |
| Feature | `<h4>`/`<h5>`/`<h6>` | `layout/layout.cpp` |

## Tests added (round 2)

- `css_display_none_parsed` — `display: none` and `text-align: center`
  parse correctly from CSS rules
- `css_text_align_inline` — inline `style="text-align: right"` works

Total self-tests: **123**, all passing (was 116 in round 1, 103 in the
bug-fix pass).

## How to test the click/hover fixes

Open `test.html` and:
1. Hover any link — the cursor should switch to a hand ✓ (was broken)
2. Click the `<div class="card" onclick="...">` — the status should
   change to "Card clicked!" ✓ (was broken when body had a background)
3. Click the link inside the card — both the onclick fires AND it
   navigates
4. Hover over a non-link area — the cursor stays as an arrow ✓
5. Click any button (e.g., `<` `>` `R` in the address bar) — works ✓

The "some links and buttons don't click" issue should now be resolved.

---

# Part 4 — Round 3: real interactive features

Round 2 fixed the click/hover bugs. Round 3 adds the features that turn
the browser from a static viewer into something approaching a real
browser: form inputs with focus, `setTimeout`/`setInterval`,
`addEventListener`, CSS `:hover`, CSS `width`/`height`, `Ctrl+L`, Tab
cycling, and a blinking caret on focused inputs.

## New feature: JS `setTimeout` / `setInterval`

A real JS engine needs timers for any non-trivial script. The engine
now keeps a `std::vector<JSTimer>` of pending timers. Each timer
stores the code to run (a string snippet, or a function name to
invoke) and a fireAt timestamp.

```cpp
struct JSTimer {
    std::string code;
    std::chrono::steady_clock::time_point fireAt;
    int intervalMs = 0;        // 0 = one-shot, >0 = repeat (setInterval)
    bool repeating = false;
    bool cancelled = false;
};
```

`setTimeout(fn, delay)` and `setInterval(fn, delay)` push a new timer.
`setTimeout(fn, delay)` accepts either a string or a `function` reference.
(The "function" must be a named function declared in the same script —
anonymous `function() { ... }` literals aren't recognized by our tiny
parser; pass a string for those.)

The Browser pumps timers every frame in `paint()` via `pumpTimers()`.
If any timer fires, the engine runs its code, which can mutate the DOM
and set `needsRelayout_ = true` via the existing `onDomMutated` /
`onInvalidate` callbacks.

## New feature: JS `function` declarations + named function calls

Required for `setTimeout(fn, ...)` and `addEventListener("click", fn)`
to be useful. The engine now:

- Recognizes `function name(args) { body }` declarations, stores the
  body keyed by name in `functionBodies_`, and registers a `JSValue::FN`
  in `globals[name]`.
- Recognizes bare `name();` calls at top level and dispatches via
  `callFunction(name)`.
- Updated `splitStatements` to also count `{}` depth, so a function
  body containing `;`-separated statements stays together as one
  declaration unit (previously it was split at every `;`).

## New feature: `addEventListener` / `removeEventListener`

Scripts can now attach event handlers without using inline `onclick`:

```js
function onClick() { console.log('clicked'); }
document.getElementById('btn').addEventListener('click', onClick);
```

Listeners are stored on the DOM node itself, in a special attribute
keyed `__listeners_<type>` with comma-separated function names. This is
a hack to avoid extending the `Node` struct.

`Browser::handleClick` calls `js_.dispatchEvent("click", target)` after
firing any inline `onclick`. The dispatcher looks up the listener
attribute and calls each named function via `callFunction`.

## New feature: CSS `:hover` pseudo-class

CSS rules with `:hover` (e.g. `nav a:hover { color: red; }`) now
match. Implementation:

- New `matchesSelectorNodeHover(selector, node, hovered)` in `style.cpp`
  — strips `:hover` from the rightmost simple selector, matches the
  rest, AND requires `node == hovered` if `:hover` was present.
- `layout::computeStyle` calls `matchesSelectorNodeHover` with
  `g_hoveredNode`, a static pointer set by `setHoveredNode()`.
- The Browser tracks `hoveredNode_` (a `shared_ptr<Node>`), updated
  in `updateHoverCursor()` whenever the mouse moves over a link or
  any box. When it changes, `needsRelayout_ = true` is set so the
  layout recomputes with the new `:hover` state.

So hovering a link now visibly changes its style according to CSS —
this is the standard browser behavior.

## New feature: `<input type="text">` / `<input type="password">`

Laid out as a small bordered text box (default 200×22 px). The `value`
attribute holds the text. Clicking gives focus, after which typed
characters append to `value` and Backspace removes the last char.

`type="password"` masks the displayed text with `*` characters.

## New feature: `<input type="submit">` / `<input type="button">`

Laid out as a small bordered button with the `value` attribute as the
label. Clicking fires the onclick handler (if any). Pressing Enter in
any text input within a `<form>` ancestor fires the form's `onsubmit`
and dispatches any `submit` event listeners.

## New feature: `<textarea>`

Multi-line text input. Initial value = the text between
`<textarea>` and `</textarea>`. `cols` and `rows` attrs set the
displayed width/height. Enter inserts a newline; Backspace removes
the last char from the text content.

## New feature: form focus management + Tab cycling

The Browser tracks `focusedNode_`. Clicking an input/textarea gives
it focus and calls `SDL_StartTextInput()` so SDL sends TEXTINPUT
events. Clicking elsewhere (or pressing Tab) moves focus.

- **Tab** cycles forward through focusable elements (inputs, textareas,
  `<a href>`, buttons)
- **Shift+Tab** cycles backward
- **Enter** in an `<input>` submits the parent `<form>` (fires
  `onsubmit` and dispatches the `submit` event)
- **Enter** in a `<textarea>` inserts a newline

The renderer draws a 2px blue focus ring around the focused input,
and a blinking blue caret at the end of the text (using `SDL_GetTicks`
modulated at 500ms on/off).

## New feature: CSS `width` / `height` / `max-width` / `min-width`

Parsed and applied to block-level boxes:

- `width` overrides the available column width
- `max-width` clamps it down
- `min-width` clamps it up
- `height` is treated as a minimum (the box never gets shorter than
  its content, but grows to the explicit height if larger)

Only `px` units supported; `%` is not.

## New feature: `Ctrl+L` focuses the address bar

Standard browser shortcut. Same as clicking the URL field, but
keyboard-only.

## New feature: `<button>` styling

`<button>` now defaults to a faint gray background and a 1px border,
matching what most browsers render for an unstyled `<button>`. CSS
can still override either.

## New feature: more heading tags

`<h4>` (18px bold), `<h5>` (16px bold), `<h6>` (14px bold) are now
styled.

## Bug fix: chained-call expression dispatch

Round 3 also fixed a real bug uncovered while testing `addEventListener`:
`document.getElementById('b').addEventListener('click', fn)` was
swallowed by the early `looksLikeCall(line, "document", "getElementById")`
check (because the line starts with `document.getElementById(`). Now
the addEventListener check runs first, and uses `extractArgs`-by-position
to grab the args of the outermost `()` instead of the inner one.

## Summary of round 3 changes

| Type | What | Files |
|---|---|---|
| Feature | JS `setTimeout`/`setInterval` + timer pump | `js/jsengine.{h,cpp}`, `app/browser.cpp` |
| Feature | JS `function` declarations + named calls | `js/jsengine.cpp` |
| Feature | JS `addEventListener`/`removeEventListener` | `js/jsengine.cpp`, `app/browser.cpp` |
| Feature | CSS `:hover` pseudo-class | `css/style.{h,cpp}`, `layout/layout.cpp`, `app/browser.cpp` |
| Feature | `<input type=text/password>` with focus | `layout/layout.cpp`, `app/browser.{h,cpp}` |
| Feature | `<input type=submit/button>` | `layout/layout.cpp` |
| Feature | `<textarea>` | `layout/layout.cpp`, `app/browser.cpp` |
| Feature | Tab/Shift+Tab focus cycling | `app/browser.cpp` |
| Feature | Form submission via Enter | `app/browser.cpp` |
| Feature | CSS `width`/`height`/`max-width`/`min-width` | `css/style.{h,cpp}`, `layout/layout.cpp` |
| Feature | `Ctrl+L` focuses address bar | `app/browser.cpp` |
| Feature | `<button>` default styling | `layout/layout.cpp` |
| Feature | `<h4>`/`<h5>`/`<h6>` | `layout/layout.cpp` |
| Feature | Focus ring + blinking caret | `render/renderer.{h,cpp}`, `app/browser.cpp` |
| Bug fix | `addEventListener` swallowed by `getElementById` check | `js/jsengine.cpp` |

## Tests added (round 3)

- `js_function_declaration_and_call` — `function f() { ... }` registers and `callFunction` runs it
- `js_function_with_body_spanning_statements` — body with two `;`-separated statements both run
- `js_top_level_function_call` — `f();` at top level runs the function
- `js_settimeout_runs_after_delay` — `setTimeout` fires on `pumpTimers`
- `js_setinterval_fires_repeatedly` — `setInterval` fires on `pumpTimers`
- `js_addeventlistener_attaches_and_fires` — `addEventListener` registers and `dispatchEvent` runs
- `css_width_height_parsed` — width/height/max-width/min-width all parse
- `css_hover_selector_matches_only_when_hovered` — `:hover` only matches when hovered

Total self-tests: **148**, all passing (was 123 in round 2, 116 in round 1, 103 in the bug-fix pass).

## How to test the new features

Open `test.html` and try:

1. **Form inputs**: click the Name field, type your name, press Tab to
   move to Password, then Tab to Comment, then Tab back. Press Enter
   in the Name field to submit the form — the status bar should update.
2. **CSS `:hover`**: hover over the nav links at the top — they should
   turn red with a yellow background. Hover over the `.card` div — its
   background should turn light blue and the border darker.
3. **JS `setTimeout`**: click "Schedule a 2-second timer". Wait 2s.
   The status bar should say "Timer fired!".
4. **JS `addEventListener`**: click the "Click me — I have a JS event
   listener" paragraph. The status bar should say "Listener fired!".
5. **CSS `width`/`height`**: the fixed box should be exactly 240x80
   pixels regardless of window width.
6. **`Ctrl+L`**: press Ctrl+L — the address bar should turn blue and
   the caret should blink. Type a path, press Enter to navigate.

## Known limitations

- Anonymous function literals (`function() { ... }`) are not supported
  as arguments to `setTimeout`/`setInterval`/`addEventListener`. Pass a
  string of code to `setTimeout`/`setInterval`, or declare a named
  function and pass its name.
- `clearTimeout`/`clearInterval` don't return timer IDs, so they're
  no-ops (with a warning). To stop a repeating timer, the script would
  need a flag instead.
- `addEventListener` only supports `"click"` and `"submit"` (the events
  the Browser dispatches). Other types register but never fire.
- Form `<label>` text doesn't actually associate with the input —
  clicking the label doesn't focus the input.
- Bare-identifier arguments in `console.log(x)` print "undefined" —
  the engine doesn't resolve variables in argument expressions. Use
  string literals or numbers in console.log.
- No `localStorage` / `sessionStorage`, no `fetch`, no `XMLHttpRequest`.
- No `<select>`/`<option>`, no `<input type="checkbox">`/`radio`.


---

## Round 4 (2.3): how element geometry reaches page JS

`getBoundingClientRect()` needs an answer to "where is this element?"
— but in this engine most elements don't own a box. Layout produces
three kinds of geometry, and a rect must be assembled from whichever
apply:

1. **Boxes** (`LayoutResult::boxes`) — one per laid-out block, atomic
   image or form widget, carrying `sourceNode` and the border-box
   (x/y/w/h) in document coordinates. Block-level elements are exact
   from this alone.
2. **Link rects** (`LayoutResult::links`) — per-line hit rectangles
   for every `<a href>`; a wrapped link has one per line it appears on.
3. **Runs** (`Box::lines[line][run]`) — the text/inline-image fragments
   inside a block's lines. These used to be anonymous; they now carry
   `Run::sourceNode`, the element that produced them (nearest element
   ancestor for text, the `<img>`/`<br>` itself for atomic runs, the
   block child for block-in-inline break markers).

`layout::rectForNode()` unions, in order: the node's own boxes, its
link rects, and — only when neither matched, since runs always live
inside their containing block's box — every run it (or a descendant
inline element) produced. Run positions are recomputed with the same
mirrored geometry `hitTestText` uses: identical font resolution
(`getFontForRun` + `measureTextCached`), identical text-align/inset
origin, identical line heights (`selectionLineTop/Height`), and inline
images flush with the line bottom exactly where the renderer draws
them. Inline elements therefore get true fragment-union rects — a
wrapped `<a>` unions to a box spanning all its lines, `<b><i>x</i></b>`
unions both elements — with no separate layout pass.

One subtlety the merge guard fixes: the wrap engine merges adjacent
same-styled runs into one. Without an owner check, `<span>a</span>
<span>b</span>` merged into a single run attributed to the first span,
erasing the second span's geometry. Merging is now same-owner-only.

The JS side never touches layout memory. `JSEngine` exposes two
provider callbacks (`rectForNode`, `scrollY`) that `Browser` wires once
to read its *current* `layout_`/`font_`/`scrollY_` at call time —
relayouts and scrolling instantly reflect in page JS, nothing dangles,
and a provider-less engine (bare `JSEngine` in tests/tools) safely
reports zero rects. The bindings subtract `scrollY` for
`getBoundingClientRect()` (viewport-relative per spec) and leave it on
for the `offset*` properties (document space).
