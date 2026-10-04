# What changed in MiniBrowser 2.0

This document covers the second round of work: the HTTP/HTTPS network
stack, the new JavaScript engine, and the bug fixes + features layered on
top. (The first round — build fixes, crash fixes, parser fixes — is
documented in `EXPLANATION.md`.)

## 1. New: HTTP/HTTPS networking (`net/`)

- **`net/fetch.cpp`** — a libcurl-based fetcher (`fetchUrl` /
  `fetchUrlCached`) with HTTPS via the system TLS store, transparent
  gzip/deflate/brotli decompression (`Accept-Encoding`), up to 10
  redirects, timeouts, a real User-Agent, and content-type extraction.
  A small per-session cache stops a page's stylesheets/scripts/images
  from being downloaded repeatedly; reload (F5) clears it.
- **`net/url.cpp`** — URL type plus RFC 3986-ish `joinUrl` resolution
  (absolute / protocol-relative / origin-relative / path-relative /
  query / fragment references), `urlEncode`/`urlDecode`, and address-bar
  normalization (typing `example.com` navigates to `https://example.com`;
  `test.html` stays a local file).
- **Pages**: navigating to `http(s)://…` downloads the document, parses
  and renders it. Non-HTML content types are handled sensibly: images
  render as an image page, `text/plain` renders as `<pre>`, everything
  else gets a notice.
- **Sub-resources**: `<link rel=stylesheet>`, `<script src>` and `<img>`
  on remote pages are resolved against the final (post-redirect) URL and
  fetched over the network. Images are decoded from memory with
  SDL_image (`IMG_Load_RW`).
- **Error pages**: HTTP failures and transport errors render a styled
  local error page ("Can't reach this page") instead of a blank screen;
  the status bar shows `HTTP 200 · text/html · 12.3 KB` style info.
- **JS networking**: `fetch(url)` (synchronous, returns
  `{ok, status, text, json(), url, contentType}`) and a minimal
  `XMLHttpRequest` (`open`/`send`, `status`, `responseText`). Both run
  on the UI thread — this browser has no worker threads by design.

## 2. New: real JavaScript engine (Duktape)

The old home-grown line-based interpreter (which only understood a
handful of statement shapes) is **replaced by an embedded Duktape 2.7
heap** (`js/duktape/`, vendored, MIT license). The `JSEngine` public API
is unchanged, so the rest of the browser barely noticed.

What pages can now do (and couldn't before):

- Real ES5.1: functions with parameters and return values, **closures**,
  `for`/`while` loops, arrays, objects, `JSON`, `Math`, exceptions with
  stack traces printed to the console.
- `document.getElementById / querySelector / querySelectorAll` (simple
  selectors: `tag`, `.class`, `#id` and combos), `createElement`,
  `createTextNode`, `document.body`, `documentElement`, `title`.
- Node wrappers (ES6 Proxies): `textContent`, `innerHTML` (get + set,
  parsed through the real HTML parser), `id`, `className`, `value`,
  `checked`, `href`, `children`, `parentNode`, `style.*` (camelCase →
  kebab CSS, e.g. `style.backgroundColor`), `setAttribute`/`getAttribute`,
  `appendChild`/`removeChild`/`remove`, `addEventListener` with **real
  function values** (closures welcome), `click()`, `focus()`.
- Events: `addEventListener` on elements AND `document`; clicks
  **bubble** (target → ancestors → document) with an `event` object
  (`type`, `target`, `targetId`); `change` fires for checkboxes/radios/
  selects; inline `onclick`/`onsubmit` still work.
- Timers: `setTimeout`/`setInterval`/`clearTimeout`/`clearInterval`
  return real IDs and accept functions or strings.
- `alert()` renders a real modal dialog; `confirm()` shows the same
  dialog (returns false for now); `console.log/warn/error` print to the
  terminal; `location.href`/`assign`/`replace`/`reload` navigate the
  browser; `location.host/hostname/pathname/search/hash/protocol` work.

## 3. Fixed bugs (this round)

1. **Compile errors**: `SDLK_F` / `SDLK_L` don't exist in SDL2 — the
   Ctrl+F / Ctrl+L shortcuts used them and the project didn't build with
   a stock SDL2.
2. **Unreachable checkbox/radio code**: in `handleClick`, the
   `input[type=checkbox|radio]` toggle lived in a *second*
   `if (tag == "input")` block *after* the first one had already
   `return`ed — so clicking a checkbox or radio button did nothing.
   All input handling is now in one place; `change` events fire too.
3. **`<script>`/`<style>` bodies were parsed as HTML**: a `<` inside JS
   (e.g. `if (a < b)`) or a `"</div>"` string literal corrupted the DOM
   tree. The tokenizer now implements RAWTEXT mode: everything up to the
   real `</script>`/`</style>` is one unparsed text token, no entity
   decoding (so JS `&` survives too).
4. **Back/forward lost your place**: history entries now store the
   scroll offset and restore it, like a real browser.
5. **Zoom didn't scale headings**: the old zoom only scaled CSS-declared
   `font-size` values; `h1`–`h6` tag defaults ignored it. Zoom is now a
   global scale applied in `computeStyle` (guarded so nesting doesn't
   double-apply).
6. **Button labels rendered twice**: `collectRuns` pulled widget text
   (`<button>`, `<input>`, …) into the parent's line AND the widget was
   laid out as its own box. Widgets are now skipped during run
   collection.
7. **Pretty-printed HTML put every inline element on its own line**:
   raw newlines between `<a>` tags were treated as `<br>`-style hard
   breaks. Newlines in normal text now collapse to spaces (only the
   explicit `<br>` run breaks; `<pre>`/`white-space: pre` is unaffected).
8. **Body background only covered the document height**, leaving the
   rest of short pages white. The body background box now stretches to
   the viewport (canvas propagation, as real browsers do).
9. **Tables**: cells started wherever the previous cell's text ended,
   producing staircase rows. Every cell in a row now starts at the same
   `y` and the row height is the tallest cell; `<table border>` draws
   cell frames.
10. **`setInterval(fn, …)` warned "not implemented"** — function-timer
    args only worked as strings in the old engine (moot after the
    Duktape swap, but it was a visible bug in the old selftest output).

## 4. Browser features added

- Smart address bar: bare domains get `https://`, file-ish inputs stay
  local; caret editing (arrows/Home/End/Delete), **Ctrl+V paste**,
  bookmark star button.
- Form **GET submits**: named inputs/checkboxes/radios/selects/textareas
  are collected into a real query string; submit buttons and Enter both
  fire `onsubmit`/listeners first.
- **Fragment links** (`#anchor`) scroll to the element (`id` or
  `<a name>`); `javascript:` URLs execute.
- **Bookmarks bar** (Ctrl+B), persisted to `~/.minibrowser-bookmarks.txt`;
  Ctrl+D / the ★ button toggles the current page.
- **View source** (Ctrl+U, or `view-source:<url>`), **save page**
  (Ctrl+S writes `<title>.html`).
- **F11 fullscreen**, **mouse back/forward buttons** (X1/X2),
  **drag & drop** an .html file onto the window to open it.
- Find bar: Enter = next, **Shift+Enter = previous**.
- Input caret is now position-aware (Left/Right/Home/End, insert and
  delete at the caret, visible caret, UTF-8 safe).
- `--screenshot out.bmp URL` one-shot headless render (uses
  `SDL_VIDEODRIVER=dummy`) — handy for CI and for the visual checks in
  this round.
- Status bar shows page info (status · content type · size) and zoom.

## 5. CSS/layout upgrades

- `rgba(r,g,b,a)` / `rgb()`, `#abc` hex shorthand, color token cleanup
  in `background` shorthand (`background: #333 url(bg.png) no-repeat`).
- `font-size` in `px`/`pt`/`em`/`%`/`rem` (em/% resolve against the
  inherited size).
- `line-height` (unitless multiplier, px, pt, em) — used by line layout,
  link hit-rects and find scrolling.
- `text-transform: uppercase|lowercase|capitalize`.
- `white-space: pre` (plus `<pre>` now truly preserves spaces and
  newlines; no word wrapping).
- `background-image: url(...)` / `background: … url(...)` — resolved
  like `<img>` (local or remote) and stretched behind the box.
- `font-weight: 100–900` (≥600 = bold).

## 6. Build

- Makefile now compiles `net/*.cpp`, `js/jsbindings.cpp`, the vendored
  `js/duktape/duktape.c` (C99), and links **libcurl** in addition to
  SDL2/SDL2_ttf/SDL2_image. Dependencies:
  `libsdl2-dev libsdl2-ttf-dev libsdl2-image-dev libcurl4-openssl-dev`.
- `make selftest` runs the unit tests (234 of them now — up from 148 —
  covering URLs, RAWTEXT tokenization, the CSS additions, the Duktape
  engine: closures, DOM mutation, events/bubbling, timers, fetch error
  paths, location, pre-layout, zoom, table alignment).
- `make screenshot URL=… OUT=…` wraps the headless render mode.

## 3. Round 3: correct-looking pages + online images (this round)

### Image loading fixed end-to-end
- **HTML entities in attributes decoded** (`html/lexer.cpp`): attribute
  values went through no entity decoding, so every URL like
  `...load.php?lang=en&amp;modules=...` kept the literal `&amp;` —
  stylesheets, scripts and images on real sites silently returned
  garbage (Wikipedia's CSS responses collapsed from 69 KB to 196 bytes).
  `parseAttrs` now runs `decodeEntities` on every value.
- **Root-relative resources on remote pages** (`layout/resource.cpp`):
  `/static/images/x.png` on `https://site/` used to resolve to a local
  filesystem path and never load; `ResourceLoader::resolve` now joins
  every relative reference against the remote page URL first.
- **Parallel image preloading** (`resource.cpp`, `app/browser.cpp`): all
  `<img>` sources (src / data-src / srcset / `<picture><source>`) are
  resolved after DOM parse and fetched with a worker pool (up to 8
  threads, 15 s timeout each) into the decoded-surface cache, so layout
  finds images instead of crawling through one sequential download.
  `curl_global_init` is now `std::call_once`-guarded for the workers.
- **Inline images render** (`render/renderer.cpp`): image runs inside
  text lines were skipped (empty text -> continue), leaving invisible
  gaps; the renderer now draws them bottom-aligned in the line box with
  alt-text placeholders, counts them in line width (text-align works on
  image lines) and honours per-line heights.
- **body/html flow like real containers** (`layout/layout.cpp`): direct
  inline children of body (text + <img> + <a>...) now flow together and
  wrap instead of each becoming a block; body background stretches over
  the whole canvas as before.
- **Floated images** inside containers are placed and registered as
  float regions so text wraps around them.

### Page fidelity
- **Tag scanner handles `>` inside quoted attributes** (`lexer.cpp`):
  Wikipedia embeds 10 KB of JSON in `data-mw='...'`; the old
  `find('>')` cut the tag at the first `>` in the JSON and leaked raw
  wikitext onto the page. The scanner now tracks quotes.
- **CSS custom properties are scoped sanely** (`css/style.cpp`): vars
  register only from root-scoped rules (`:root`, `html`, `body`, `*`)
  and only when their `@media` condition holds — Wikipedia's
  `html.skin-theme-clientpref-night { --dark-vars }` no longer turns
  the whole page black, and `(prefers-color-scheme: dark)` blocks are
  skipped entirely.
- **`background: transparent` no longer paints opaque black** (the
  renderer forced alpha 255 and rgba(0,0,0,0) became solid black).
- **`visibility: hidden`** supported end-to-end (parse, cascade,
  subtree skip at layout, paint skip) — Wikipedia's dropdown menus stay
  closed. `height: 0` also collapses containers now.
- **Flex fix**: whitespace-only text nodes between flex items are no
  longer treated as full-width items (this alone had pushed Wikipedia's
  whole header off-screen by one container width per level).

### Performance (real-world loading)
Wikipedia's Boston article: from *unusable* (280 s+ / never finishing)
to **~5 s end-to-end**, layout pass from 26 s to **~0.5 s**:
- Rule buckets: selectors are pre-parsed once (`selTokens`, `selKeys`,
  fully parsed `selItems`) and bucketed by rightmost tag/class/id, so
  each node only visits candidate rules instead of every rule.
- @media conditions evaluated once per ruleset, not per node.
- Per-pass computeStyle memo (node + inheritable-base fingerprint) with
  per-node multi-entry slots; 94% hit rate on Boston.
- Ancestor-fingerprint Bloom filter prunes descendant selectors
  (`.a .b *` fails instantly when no ancestor has the left part).
- Allocation-free hot-path matching (`matchesSelectorItems`), plus a
  manual whole-token class check replacing a per-call `istringstream`.
- One computeStyle per child per container (was up to 4x per child).
- No second layout pass when no script ran.

### Testing
- `--selftest` covers the new pieces (URL, CSS scoping, layout);
  **238/238 tests green**; verified against live HTTPS sites
  (example.com, Hacker News, Wikipedia incl. images + CSS) with the
  headless `--screenshot` mode (window size overridable via `MB_W`/
  `MB_H` env vars for tall-page captures).

## Session 3 — RTL/BiDi polish verification, popular-site fixes, speed

### Correctness on real sites (all verified with headless screenshots)
- **Floats never overlap text any more** (`layout.h/.cpp`,
  `render/renderer.cpp`): per-line float insets are now recorded during
  wrapping (`WrapLine::insetLeft/Right` → `Box::lineInsets`) and applied
  by the renderer, so lines flowing beside a floated image start (LTR) /
  end (RTL) inside the shrunken span. Previously every line started at
  the box edge and painted straight through left-floated images (the
  en-wiki featured-article image, "In the news" portraits). Link
  hit-rects use the same inset.
- **Block-in-inline keeps its block semantics** (`layout.cpp`): a block
  element inside an inline element (`<span><div style="float:right">…`,
  how Wikipedia wraps every thumbnail) is recorded as a flow spot and
  laid out as a real block (floats register their region) instead of
  being flattened into the text flow. Document order is preserved by
  splitting the inline run stream at hard-break markers and interleaving.
- **CSS grid line-based placement** (`css/style.h/.cpp`,
  `layout.cpp`): `grid-column` / `grid-row` with `<start> / <end>`,
  `span N` and negative (from-the-end) lines, numeric `grid-area`
  2-4 value forms, auto-placement that skips occupied cells and clamps
  spans. BBC's `repeat(24,1fr)` page grid used to collapse every item
  into a 21 px sliver; the page now lays out like the real site
  (main 18 cols + rail 6 cols, 4-up card rows).
- **`input type=hidden` renders nothing** (Google's search form shipped
  a dozen stray boxes), and submit-button labels measure through the
  run font path so CJK values ("Google 搜尋") size correctly.
- **CJK text renders again** (`font_loader.cpp`): this SDL_ttf/FreeType
  build rasterizes the NotoSansSC variable font (`NotoSansSC[wght].ttf`)
  as a completely transparent surface — every Chinese/Japanese glyph on
  the web painted invisible. The CJK fallback list now prefers static
  fonts (NotoSerifSC Regular, wqy-zenhei, Sarasa Mono SC, LXGW WenKai).

### Performance
- **Parallel `<script src>` prefetch** (`app/browser.cpp`): external
  scripts were downloaded one-by-one on the critical path (BBC spent
  ~3.4 s there). They now download through an 8-thread pool before the
  sequential execution walk. BBC total load: **4.1 s → 0.76 s**.
- **JS execution budget**: scripts run in document-order chunks and
  stop after 1500 ms (MB_JSBUDGET to tune, 0 = off) — tracking payloads
  on heavy pages no longer delay the render.
- **Selector buckets scale to 35k-rule stylesheets** (`css/style.cpp`,
  `layout.cpp`): attribute-only selectors (`[data-…]=` — GitHub/primer
  ships tens of thousands) are bucketed by attribute name instead of
  being universal candidates; `:root` rules are visited only by the root
  node; rightmost pseudo-elements (`::before`, `::placeholder`, …) and
  empty `:is()` are skipped entirely; interactive pseudo-classes
  (`:hover`/`:focus`, ~1500 in primer) are a separate bucket consulted
  only during hover/focus relayouts. GitHub's layout went from an
  **infinite hang (>3 min) to ~2.4 s**.
- **Default-styled text measures real widths**: `getFontForRun` falls
  back to the cached 16 px regular font instead of null when the caller
  has no default font — plain 16 px text used to measure every word at
  0 px, which silently disabled wrapping for it (and fed the float
  overlap bugs).

### Verification
- 275/275 selftests green (BiDi/shaping tests included).
- Screenshot checks: arabic.html (shaping, mirroring, numbers, dir=auto,
  LTR-in-RTL), cjk-test.html, example.com, Hacker News, Google (zh-HK
  UI renders), en.wikipedia Main Page (floated thumbs wrap, no
  overlaps), ar.wikipedia Main Page, BBC News (page grid + card rows),
  GitHub (renders; was a hang). reddit.com 403s datacenter IPs (server
  side, not a renderer issue).


## 4. Round 4 — GitHub/Reddit fidelity and speed ("fast and correct")

### Correctness: layout engine
- **flex-column items never moved into place** (`layout.cpp`): the column
  branch computed the target y into a local and dropped it, leaving every
  `flex-direction: column` subtree painted at the top of the page. GitHub's
  hero, nav and forms all stacked on the banner; this one shift fixes the
  entire class.
- **Out-of-flow elements no longer participate in flow**: `position:
  absolute/fixed` children were laid out by the normal block/flex/grid
  loops AND again by the absolute pass — advancing the flow cursor by
  their full height and painting them twice (GitHub's fixed header
  wrapper consumed ~430 px of flow). They are now skipped everywhere and
  placed once by the dedicated pass, which also honors declared
  width/height instead of only the measured extent.
- **sr-only / visually-hidden elements render nothing**: new `overflow`
  (hidden/clip/auto/scroll) and `clip-path`/`clip` support. `inset(>=50%)`
  and zero-area `rect(...)` clips skip the subtree like `visibility:
  hidden` (GitHub's "Navigation Menu", Wikipedia's "Jump to content",
  `.sr-only` galleries); explicit `width:1px;height:1px;overflow:hidden`
  boxes keep their declared size instead of the 40 px floor and are
  clipped to it at paint time. Inline-run collection skips visibility/
  clip/out-of-flow elements, so hidden text no longer leaks into the
  parent's text stream.
- **Sibling combinators (`a + b`, `a ~ b`) match again**: the selector
  Bloom-filter pruning assumed every non-rightmost compound is an
  ancestor. Sibling compounds never are, so every rule with `+`/`~` was
  silently dropped (Wikipedia's `.cdx-button--icon-only span + span`
  sr-only clip, hundreds of rules on every modern site). Selectors with
  sibling combinators now take the full-match path.
- **`:not(a,b)` selector lists and `:defined`** (`style.cpp`): `:not()`
  compared its argument as ONE selector, making
  `:not(faceplate-…,…)` always-true; `:defined` was unknown, so
  `:not(:defined)` matched EVERY element. Reddit's
  `:not(:defined):not(faceplate-list){visibility:hidden}` gate blanked
  the whole page. Lists match per-alternative now and `:defined` is true
  for all standard elements.
- **`<style/>` self-closing**: per HTML parsing the slash is ignored —
  `<STYLE/>` used to become an empty element and dump its raw CSS onto
  the page as text (Reddit's block page starts with one).
- **Negative `top`/`left` offsets** (BBC's `top:-9999px` skip link) were
  already parsed; the leak was the inline-run copy painting the anchor at
  the flow position as well. `collectRuns` now skips out-of-flow inline
  elements (the absolute pass owns them).
- **HTTP error pages with a body render** (like real browsers): reddit.com
  answers datacenter IPs with HTTP 403 + a full "You've been blocked by
  network security" page — previously replaced by the local error page.

### Correctness: parsing
- **`data:` URI images** decode through the normal image path (Reddit's
  block-page Snoo is a giant base64 PNG).
- Lexed attribute values keep entity decoding; `<STYLE/>`-style raw text
  now tokenizes correctly (see above).

### Performance
- **Stylesheets fetch in parallel** (8-thread pool, deduped by resolved
  URL): GitHub ships 29 `<link rel=stylesheet>` (~5.6 MB with repeats);
  sequential fetching cost ~4.5 s. Now ~1.6 s, and dedupe cuts the CSS
  handed to the parser from 5.6 MB to ~2.4 MB, which roughly halves
  computeStyle work (layout 1.7 s -> 0.5 s).
- **No second layout when no script actually ran**: the post-JS relayout
  keyed on "scripts were attempted"; duktape rejects modern bundles
  instantly, so GitHub paid a full second layout for nothing. Only
  successfully-executed scripts trigger it now. `JSEngine::execute`
  returns whether the script ran.
- **github.com totals: ~25 s (with the earlier hang, >3 min) -> ~2.3 s**
  end-to-end including network.

### Verification
- 275/275 selftests green.
- Live screenshots re-verified: google, en/ar wikipedia (RTL shaping
  intact), Hacker News, example.com, BBC News (grid + cards; skip link
  now hidden), github.com (banner/hero/features/customers all in place,
  no overlaps), reddit.com (faithful render of the server-side 403 block
  page: data-URI image, centered text).
- Known remaining: github full pixel fidelity (icon sprite columns, the
  hero carousel area is text-only), BBC nav renders as a stacked list
  (collapsed grid column), ar.wikipedia template internals, Korean Hangul
  tofu (no Hangul font installed).

## Round 5: the invisible-chrome bug (dwm report) + URL in the window title

### Bug: the whole UI chrome never reached the screen under a real X11 WM
- **Symptom**: under dwm the window showed page content only — no address
  bar, no status bar, no scrollbar, no find/bookmarks/alert UI. The URL
  was therefore invisible (dwm shows only the window title).
- **Root cause** (`render/renderer.cpp`): the content passes `render()` /
  `renderWithFocus()` started with `SDL_RenderClear(white)` and ended
  with `SDL_RenderPresent()`. `Browser::paint()` calls the content pass
  FIRST and draws the chrome AFTER it, so every frame the half-drawn
  frame (content, chrome not yet drawn) was pushed to the display, and
  the chrome that paint() drew afterwards sat in the backbuffer until the
  next frame's `SDL_RenderClear` wiped it. The chrome was never visible
  on screen — only in `--screenshot` BMPs, which read the backbuffer
  directly after the full frame, masking the bug in all offline checks.
- **Fix**: content passes no longer clear or present — the frame owner
  (`Browser::paint`) clears, draws content, draws chrome, then presents
  exactly once per frame. Also saves one redundant full-surface clear
  per frame. (Same bug also hid JS `alert()` modals and the find bar in
  interactive use.)

### Feature: URL in the window title (dwm-friendly)
- New `Browser::updateWindowTitle_()` composes `"<doc title> — <URL>"`
  (or just the URL for untitled pages) and is used by every title
  update site: remote loads, local files (which never updated the title
  at all before), about:home/blank, view-source, and the loading/error
  states (`"Loading... <url>"`, `"Error — <url>"`).
- The URL is now visible in three places: the dwm/WM bar (window
  title), the address bar, and the status bar.

### Verification
- Reproduced the chrome-less window under Xvfb with a live event loop +
  `ffmpeg -f x11grab` captures; confirmed the fix restores the address
  bar / status bar / scrollbar interactively (pixel checks on the gray
  232/232/236 bar, buttons, URL text, status strip).
- Window title verified via XFetchName: `Example Domain — https://example.com/`.
- 275/275 selftests green; live sweep re-verified (www.wikipedia.org,
  en/ar wikipedia, Hacker News, BBC, github) — pixel-identical content
  modulo the 54 px chrome offset.

## Round 6: JS-only sites (google search / youtube), weak-device optimization

### New: site shims (`app/shims.{h,cpp}`) — MB_NOSHIM=1 disables all
- **Web search works**: google.com/search ships a JS-only shell (basic
  HTML was retired by Google; even `gbv=1` and text UAs get 3 links).
  `google.com/search?q=…` now rewrites to DuckDuckGo Lite — fully
  server-rendered, ~20 KB, results/snippets/pagination render and are
  clickable. Bare phrases typed in the address bar ("hacker news") are
  treated as a web search the same way (URLs, domains and file paths
  unchanged).
- **YouTube lite pages** (all three page kinds):
  - `/results?search_query=…` and `/watch` carry their content inside
    the page's own `ytInitialData` / `ytInitialPlayerResponse` JSON.
    The shim extracts those blobs and walks them with duktape
    (ES5.1-only walk, depth-bounded), then renders a static page:
    thumbnails (i.ytimg.com mqdefault), titles, channel, duration,
    view counts — in English (the fetcher now sends
    `Accept-Language: en-US,en;q=0.9`; `MB_LANG` overrides).
  - `/` (home) is consent-gated server-side for datacenter/no-cookie
    clients (dialog-only ytInitialData); the shim renders a useful
    page: search box + category shortcuts.
  - Watch pages show title/channel/length/views/description; video
    playback is explicitly out of scope for this engine.
  - Thumbnail layout uses the float-wrapper-div pattern (float on an
    `<img>` inside `<a>` is dropped by the inline-run collector —
    matching where wikipedia thumbs work).
- View counts get digit grouping (181422437 -> 181,422,437).

### Optimization: weak-device battery/CPU
- **Event-driven repaint**: the main loop used `SDL_PollEvent` +
  full-window `paint()` + `SDL_Delay(16)` — i.e. 60 full-page repaints
  per second forever, even with zero input (measured ~10% CPU idle on
  the test box; much worse where painting is slow). The loop now uses
  `SDL_WaitEventTimeout` with `Browser::nextWakeupMs()`: 0 when a frame
  is pending, the JS-timer deadline, the caret-blink tick while
  editing, else -1 (block). `Browser::tick()` fires timers and runs
  deferred work; `frameDirty_` gates repaints; every event marks the
  frame dirty (one repaint per drained batch). **Measured idle CPU:
  ~10% -> ~0% (process sleeps in sigsuspend, zero wakeups over 3 s).**
- **Deferred page scripts**: `showPage_` collects `<script>` chunks but
  no longer fetches/executes them on the critical path. The first
  frame (chrome + content) presents right after CSS + layout; the
  parallel `<script src>` prefetch + budgeted duktape execution
  (`runPendingJs_`) run on the next loop tick, then one relayout if a
  script actually mutated the DOM. First paint on JS-heavy pages no
  longer waits for MB_JSBUDGET (default 1500 ms). `--screenshot` runs
  tick/paint twice so JS-driven content stays reproducible headlessly.
- **Image decode cap** (`net/fetch.cpp`, MB_MAXIMG overrides, 0 = off):
  surfaces larger than 1280 px on their long edge are downscaled at
  decode (SDL_BlitScaled after RGBA32 conversion). A 4000x3000 photo
  no longer becomes a ~48 MB surface + texture upload for an image
  that renders into a few hundred CSS pixels.

### Verification
- 275/275 selftests green; wikipedia content pixel-identical to round 4,
  BBC identical modulo rotating content, local arabic.html RTL test
  unchanged (a live ar-wiki diff was a Wikimedia 429 rate-limit page —
  server-side).
- Live checks: google.com/search?q=… -> DDG Lite results render fully;
  address-bar phrase search works; YouTube search page with 20/20
  thumbnails; watch page metadata + description; home fallback page;
  interactive idle CPU 0.0% over sustained sampling.
- The `Makefile` is now included in browser-improved.zip (it was
  missing from earlier archives).

## Round 7: "it really needs to be fast" — async everything, viewport
## culling, measure cache, GPU renderer, and mpv video hand-off

The round-6 work made the browser *idle* cheap (event loop) but page
loads still froze the UI and every frame re-painted the whole page.
This round removes every remaining main-thread block and makes each
frame paint only what is visible. Video playback is delegated to mpv
(external player), with the web view kept for description/related
links — exactly as requested.

### What was slow, and what replaced it

1. **Blocking document fetch** (`app/browser.cpp`): `loadRemotePage_`
   fetched the main document synchronously on the UI thread (up to the
   full curl timeout). Now the fetch (+ the YouTube ytInitialData→HTML
   conversion, which builds its own duktape heap and is thread-safe)
   runs on a detached worker thread; the result lands in a
   generation-guarded slot that `tick()` polls. Stale results from
   superseded navigations are discarded by sequence number.
   The status bar shows "Loading…" immediately and the UI stays
   interactive the whole time. `--screenshot` opts into the old
   blocking behavior via `setSynchronousNavigation(true)`.

2. **Blocking image preloads** (`layout/resource.{h,cpp}`): the old
   `preloadImages()` spawned a pool and JOINED it inside `showPage_`,
   so first paint waited for every image on the page. Replaced by a
   persistent background pool (`startPreload`, MB_IMGT caps workers,
   default 3 — weak devices don't benefit from more) and:
   - `loadImage()` on a remote miss now ENQUEUES a job and returns
     nullptr — the grey alt-text placeholder shows until it lands;
     nothing ever downloads on the layout/paint thread again.
   - Failures are negative-cached, so dead URLs can't loop.
   - `Browser::tick()` polls `consumeImagesArrived()`: repaints at
     once and re-flows with a 300 ms debounce (plus a final pass when
     the queue drains) — a landing gallery costs at most a couple of
     relayouts, not one per image.
   - `waitForPendingImages()` exists only for the screenshot tool.

3. **A TLS+TCP handshake per transfer** (`net/fetch.cpp`): every request
   used to create and destroy a curl easy handle, so all 73 Wikipedia
   images each paid a fresh connection. Handles are now thread-local
   and reused (`curl_easy_reset` between uses keeps the connection
   cache alive); short-lived worker threads release theirs explicitly.

4. **Whole-page repaint per frame** (`render/renderer.cpp`): every
   paint walked ALL boxes/lines/runs — on a 2000-box page that was
   200+ ms per frame, scrolling included. Boxes outside the viewport
   (64 px margin, plus a horizontal check) are now skipped outright,
   and inside tall boxes the line loop skips above-viewport lines and
   breaks at the first line below it (lineY grows monotonically).

5. **Re-measuring every word on every relayout**
   (`layout/font_loader.cpp` + 10 call sites in `layout/layout.cpp`):
   added `measureTextCached()` — a bounded (font, text) -> width cache
   (60k entries). Repeat relayouts (JS edits, zoom, image arrivals)
   skip nearly all TTF_SizeUTF8 calls.

6. **Software renderer** (`main.cpp`): the window renderer was
   hard-coded SOFTWARE. Now ACCELERATED (+vsync) first, then
   ACCELERATED, then SOFTWARE — weak devices with any working GL
   driver get GPU blitting of the cached text/image textures; headless
   boxes fall back unchanged. Nearest-neighbour filtering (hint)
   keeps blits 1:1 cheap.

7. **-O3 + -flto=auto** (`Makefile`): layout/shaping/decode are pure
   CPU on the target device; LTO also prunes cross-TU dead code.
   (Makefile recipe tabs restored — they had been corrupted to spaces.)

### mpv video hand-off (YouTube and direct media)

- New `app/mpv.{h,cpp}`: PATH lookup (cached), `play()` = fork/exec of
  `mpv --no-terminal --really-quiet <url>` in its own session
  (setsid; output to /dev/null; SIGCHLD auto-reap) — closing the
  browser never kills playback, and the UI never blocks.
- YouTube watch pages (synthesized from ytInitialPlayerResponse) now
  carry a red "▶ Play in mpv" button linking the pseudo-scheme
  `mpv:<watch-url>`, plus a "or press v" hint. The page itself keeps
  title, author, duration, view count, description and "Up next".
- `navigate()` intercepts: `mpv:` URLs (play, stay on page) and direct
  media links (.mp4/.mkv/.webm/.m3u8/.mpd/.avi/.mov/.ts/.ogv/.flv/
  .mp3/.m4a/.flac/.wav/.opus/.ogg, query/fragment-insensitive,
  case-insensitive) — the browser no longer downloads a whole video
  just to print "Unsupported content type".
- Keyboard: **v** plays the current page when it is a video (or shows
  a hint on non-video pages / when mpv is missing: "install mpv (and
  yt-dlp for YouTube)"). MB_YTAUTO-style autoplay was deliberately NOT
  added: the web view (comments/description) stays the primary view.

### Performance, measured (this sandbox, fast network)

Wikipedia article (73 images), document-arrival to ready-for-paint:
- old: cssFetch+imgPreload 2461 ms (joined all 73 images) + layout
  -> ~3.2 s frozen UI. new: 234 ms (async kick-off) -> ~1.1 s to
  first paint, UI alive throughout. On slow links the old number grew
  without bound; the new one doesn't include images at all.
- 2000-paragraph benchmark page, per-frame paint (MB_PAINTBENCH):
  182-307 ms (old, any scroll) -> 1-3 ms (new, any scroll) — ~100x.
- Repeat relayout of the same page (JS mutation): 297 -> 210 ms
  (-31%: measure cache + O3 + LTO).
- Idle CPU: unchanged ~0% (event-driven loop from round 6).
- Rendering equivalence: old vs new screenshots are pixel-identical
  (0/819200 differing pixels) on both the wikipedia article and the
  benchmark page, at scroll 0 AND scroll 9000 — culling and the
  measure cache change speed only, not output.

### Verification
- 286/286 selftests green (275 prior + new mpv::isMediaFile and
  ResourceLoader async-API tests).
- Live screenshots: youtube watch page (Play button + full metadata),
  youtube home fallback, DDG Lite results, Hacker News, GitHub,
  wikipedia — all correct; github's ES6 bundle still logs a harmless
  duktape parse error (pre-existing).
- Xvfb interactive run: `[render] using opengl renderer`, window +
  chrome + "Loading…" visible at t=1.2 s while the document was still
  in flight; title updated to "United Kingdom - Wikipedia — <url>"
  on completion.
- mpv itself is not installed in this sandbox; the integration is
  exercised via unit tests and the graceful "mpv not found" status
  path. On the target device: `apt install mpv yt-dlp` (or distro
  equivalent) is all the user needs.

## Round 7 — mpv launch reliability + fast-navigation wakeup race

User report: the "Play in mpv" button renders but clicking it never
launches mpv. Two independent root causes were found and fixed.

### Fix 1: silent mpv death (the reported bug)
The launcher used to exec mpv detached with stderr/stdout -> /dev/null,
then immediately claim "Playing in mpv: <url>" in the status bar. On a
real machine the overwhelming failure mode is: mpv starts, fails to
resolve the YouTube stream (yt-dlp missing, stale extractor blocked by
YouTube, python missing...) and exits within a second — silently. The
user sees nothing happen.
- app/mpv.{h,cpp} rewritten:
  * exec the FULL resolved path (PATH scan cached) — playback no longer
    depends on the child inheriting a sane PATH;
  * mpv's stderr/stdout go to a diagnostic log (TMPDIR or /tmp/
    mini-browser-mpv.log, truncated per launch) with a header line;
  * --really-quiet dropped (it also swallows the diagnostics we now
    capture); --no-terminal kept;
  * new helpers: alive() (kill(pid,0) probe; SIGCHLD stays SIG_IGN so
    the kernel auto-reaps and dead pids report ESRCH), logTail(),
    ytDlpAvailable() (yt-dlp OR youtube-dl on PATH), needsYtDlp()
    (youtube.com/m./music./youtu.be/youtube-nocookie.com hosts,
    suffix-matched — notyoutube.com is correctly rejected).
- app/browser.cpp: maybePlayInMpv_ now
  * refuses YouTube URLs up front when no extractor exists: "mpv needs
    yt-dlp for YouTube — install it (e.g. pip install yt-dlp), then
    press v again" (this was almost certainly the reporter's case);
  * on launch says "Starting mpv: <url>" and schedules a liveness probe
    (mpvCheckDueMs_, honoured by tick() and nextWakeupMs()): after ~2 s
    the status bar either confirms "Playing in mpv" or shows
    "mpv exited: <first lines of mpv's log>".
- resolveLink_ passes mpv: pseudo-URLs through untouched (the generic
  joiner would mangle them into a path of the current page on local
  documents).

### Fix 2 (found while e2e-testing): "Loading…" forever on fast pages
nextWakeupMs() returned 8 ms only while doneSeq != navSeq_; a fetch that
completed BETWEEN loop iterations (localhost, cache hits — anything
faster than one 8 ms poll) fell through to the idle sleep (-1 = forever)
before tick() could consume the result, so the page hung on "Loading…"
with no event ever arriving. Slow internet sites always finished inside
a poll window, which is why this never showed on youtube/wikipedia.
- nextWakeupMs() now locks the nav slot and returns 0 whenever a
  completed-but-unconsumed result is present (NavSlot::m became mutable
  for the const wakeup check). Fast pages now render within one loop
  pass (~40 ms total for the local test server page).

### Test infrastructure
- New env-gated hook MB_AUTOCLICK="x,y" (content coords): fires ONE
  synthetic click through the real handleClick() once the page has
  settled (first paint + initial navigation consumed + 300 ms), so the
  full click -> navigate -> mpv chain is e2e-testable without a pointer.
- Fake mpv/yt-dlp shell scripts (scripts/fakebin/) record their argv and
  can simulate instant death (FAKE_MPV_MODE=fail).

### Verification (all in Xvfb, fake binaries)
- Real YouTube watch page (Rick Astley, live fetch): click on the
  button -> fake mpv received exactly
  `--no-terminal https://www.youtube.com/watch?v=dQw4w9WgXcQ`,
  process stayed alive, status bar progressed to "Playing in mpv".
- yt-dlp missing: NO launch; status "mpv needs yt-dlp for YouTube —
  install it (e.g. pip install yt-dlp), then press v again".
- mpv dies (FAKE_MPV_MODE=fail): status becomes "mpv exited:
  ERROR: ytdl: yt-dlp not found or too old" (real log tail).
- Local-host page regression: loaded and interactive (was: stuck on
  "Loading…" forever in round-7-rc AND in the round-6 deliverable —
  pre-existing bug, now fixed).
- 299/299 selftests (+13 new assertions for path()/alive()/needsYtDlp/
  logTail semantics); idle paints over 11 s: 3 (event loop still
  sleeps); wikipedia + example.com + DDG search screenshots unchanged.

## Round 6.2 — "no video / messed-up rendering / URL gone" report

User symptoms on their machine: no video after clicking Play, page
rendering mush, and the URL no longer visible. Root causes found and
fixed:

### Fix 1: YouTube shim could fall back to the raw JS shell
youTubeLiteHtml() returned "" whenever ytInitialPlayerResponse /
ytInitialData were missing (consent gates, region variants, bot walls
serve different HTML). The engine then rendered the untouched 2 MB
YouTube app shell — unreadable mush, and every page looked broken.
- Watch pages now ALWAYS return a clean lite page: the Play button is
  built from the video id in the URL (never from the HTML), so it works
  on every variant; a note replaces the description when metadata can't
  be parsed.
- Search pages with no parseable results get a clean "try another
  query" page instead of the raw shell.

### Fix 2: author link styles were stomped by the UA default
computeStyle applied inline/stylesheet declarations FIRST, then
unconditionally reset every <a href> to blue+underlined — so the
author-declared colors (the shim's white-on-red Play chip, any real
site's CSS link colors) never showed.
- The UA default now yields to anything the cascade declared
  (s.declared bits): B_COLOR / B_UNDERLINE.
- text-decoration:none records its bit explicitly (diffBits only
  records CHANGED fields, so "none" on a not-underlined default used
  to vanish — the Play button kept its underline).
- Play button rebuilt as a div chip (block backgrounds render) with a
  white, non-underlined anchor inside.

### Fix 3: status bar collision on long guidance messages
The yt-dlp install hint ("mpv needs yt-dlp for YouTube — install it …")
drew over the left-side URL. The left URL now truncates middle-out
(keeps scheme + tail) to the space the right message leaves.

### Fix 4: explicit expose/restore repaint + build identity
- SDL_WINDOWEVENT_EXPOSED / RESTORED / SHOWN set frameDirty_ (dwm
  occlusion, e.g. a fullscreen mpv window, then switching back).
  Verified by cover/uncover pixel-diff: 0 pixels differ.
- Startup stderr banner "[mini-browser] 2.1-round6 built <date time>"
  and about:home header bumped to 2.1, so a stale binary is instantly
  recognizable.

### Verification (Xvfb, live network, fake mpv)
- Real YouTube watch page: red Play chip renders (white text, no
  underline); auto-click -> fake mpv receives
  `--no-terminal https://www.youtube.com/watch?v=dQw4w9WgXcQ`; status
  "Playing in mpv"; window title and URL bar carry the full URL.
- No yt-dlp on PATH: NO launch; status shows the install hint next to
  a truncated URL (no overlap).
- Cover/uncover (mpv-style occlusion): pre/post screenshots identical.
- 307/307 selftests (new: youtube_shim_never_raw_html — junk HTML must
  still yield a clean watch page with the mpv: link, no shell leak).
- Site regressions: wikipedia / HN / GitHub / example.com unchanged;
  DDG timeout was sandbox network, error page renders correctly.

## Round 6.3 — Arabic float-crush fix, dwm title cap, YouTube consent cookies

### Fix 1: Arabic/sidebars crushed to one-character columns (layout.cpp)
Root cause chain found on live ar.wikipedia:
- A gadget rule `.mw-header .search-toggle { float:left }` floated the
  header search button. Flex items are independent BFCs (CSS Flexbox
  §4) — the float must never leave the header — but the engine shared
  one FloatRegion vector down the whole tree, so it leaked and was
  visible to every narrow box on the page (153 crush victims logged).
- Narrow blocks fully covered by a float wrapped at the 20px floor ->
  Arabic words broken into 2-char vertical fragments ("الر/ئي/سي/ة").
- Fixes:
  * flex/grid containers now lay out with an isolated floats vector
    (BFC semantics; also CSS Grid §4).
  * CSS 2.1 §9.5.2 "line boxes shift down": when floats crush a regular
    block's usable width under 100px (and overlap its first lines), the
    box drops BELOW the overlapping floats instead of wrapping into
    one-character columns. Same guard for <li>. Sanity cap 2000px.
  * Verified: ar.wikipedia main content readable (was blank/mush),
    isolated float test (artest.html) matches spec, 307/307 selftests,
    wikipedia/HN/GitHub/DDG/BBC/example.com regressions clean.

### Fix 2: dwm bar truncation hid the URL (app/browser.cpp)
dwm hard-truncates an overflowing title bar at the END; video pages
ship 100+ char titles, pushing the URL out of the bar ("the url is not
showing again"). updateWindowTitle_() now caps the document title at 64
UTF-8-safe chars (never cutting inside a multi-byte glyph) and appends
"…", so "Title… — URL" always fits. Verified via xwin_titles with the
exact video title from the user's screenshot.

### Fix 3: protocol-relative CSS URLs failed (app/browser.cpp)
`<link href="//fonts.googleapis.com/...">` (duckduckgo html results,
many sites) reached curl scheme-less and failed ("[css] could not
load"). resolveLink_() now inherits the document scheme (https default)
for "//host/path" references. DDG html results: 0 CSS failures (was 2),
topic card + results render.

### Fix 4: YouTube consent-gate / bot-check hardening (net/fetch.cpp)
- YouTube requests now send `Cookie: SOCS=CAI; CONSENT=YES+...;
  PREF=hl=en&gl=US` (plus the existing Accept-Language): pre-accepts
  the EU consent wall so ytInitialPlayerResponse/ytInitialData are
  served without JS.
- Datacenter/VPN IPs still get playabilityStatus LOGIN_REQUIRED ("Sign
  in to confirm you're not a bot") — that is genuine server-side
  bot-blocking, not fixable client-side. The lite page now surfaces the
  exact reason: "This video's metadata could not be read — YouTube
  says: Sign in to confirm you're not a bot. Playback still works…"
  (mpv/yt-dlp uses its own client and plays it anyway).
- e2e re-verified with fake mpv: lite page renders with real title
  "Rick Astley - Never Gonna Give You Up…" + author/views when the
  page passes the gate; auto-click hands the watch URL to mpv; status
  "Playing in mpv"; cover/uncover pixel-diff 0; title bar carries URL.

### Fix 5: JS-marker class for progressive enhancement (html/parser.cpp)
The engine always ships a JS runtime, so the root <html> element now
gets "client-js" appended (as MediaWiki's own JS would), enabling
`.client-js …` enhancement rules on real sites.

### Performance state (weak-device focus)
- -O3 + LTO build (Makefile).
- YouTube watch pages: 1.5ms layout on the ~1-3KB lite page (the raw
  2MB shell with its 4s JS burn + 4s reflow never reaches the engine
  with the current binary — a stale binary DOES and is the "messed up
  rendering" the screenshot showed; rebuild required).
- Image-arrival relayouts already debounced (300ms window + final
  drain); per-thread keep-alive curl handles; text/texture caches on.

---

# What changed in MiniBrowser 2.2 (round 3 of improvements)

This round focused on web compatibility, the JavaScript API surface, and
a flagship UX feature. All of it is covered by new self-tests: the suite
grew from 307 to 413 tests, all green.

## 1. Build fixes (the project did not build as shipped)

- **Makefile recipes used spaces, not tabs.** GNU make rejects the file
  outright ("missing separator"). All recipe lines are tabs now.
- **`pkg-config --cflags` was never consulted.** Only `--libs` was, so
  any SDL installed outside `/usr/include` (brew, a custom prefix, CI
  containers) failed with `SDL2/SDL.h: No such file or directory`. The
  header paths now come from pkg-config like the library paths always
  did.

## 2. HTML5 implied end tags (`html/parser.cpp`)

Real-world HTML leans on the parser to close tags implicitly. The old
tree builder nested everything: `<li>a<li>b` produced one `<li>`
containing the rest of the list, `<p>one<p>two` one paragraph inside
another, `<tr><td>x<td>y<tr>` a table with a single row. The parser now
implements the pragmatic subset of the HTML5 tree-construction rules:

- new block-level start tag closes an open `<p>` (through inline
  wrappers — `<p><b>bold<p>next` — with HTML5 button-scope boundaries);
- new `<li>` closes the previous `<li>` scoped by `<ul>/<ol>`;
- new `<dt>/<dd>` closes the previous one scoped by `<dl>`;
- `<tr>`, `<td>/<th>` and `<thead>/<tbody>/<tfoot>` close their open
  predecessors scoped by `<table>` (an implied `</td>` also closes its
  `<tr>` transitively);
- new `<option>/<optgroup>` closes open ones scoped by `<select>`.

Result: lists, paragraphs, tables and selects from typical
hand-written/CMS pages now lay out exactly as a real browser sees them
(see `round7.html` for a live demo page).

## 3. Named entity table (~160 entries, `html/lexer.cpp`)

The old decoder knew 10 entities via an if-chain. A sorted, binary-
searched table now covers typographic quotes, arrows, currency, math
symbols, Greek letters, Latin-1 accents and more — the entities that
actually appear on Wikipedia/blog content. The classic semicolon-less
forms (`&amp`, `&lt`, `&gt`, `&quot`, `&copy`, `&reg`, `&nbsp`) decode
when followed by a non-name character, but never mangle words like
`&ampersand`.

## 4. `<base href>` support

Pages served from non-root paths (common with CMS installs) re-anchor
every relative reference with `<base href>`. The first one in the
document now overrides the base for links, form actions, stylesheets,
scripts and images (`Browser::effectiveBaseUrl_` + `findBaseHref`),
including local pages whose base points at a URL.

## 5. Text selection with copy to clipboard

- **Drag** with the left button on plain text/background to select;
  the drag endpoint is tracked live and the covered runs paint with a
  translucent blue rectangle (`SelectionSpan` in `render/renderer.h`).
- **Double-click** selects the word under the cursor (drag from there
  extends it).
- **Ctrl+C** copies the selection (logical text order, bidi-runs use
  their original text); any press on a link/widget or a navigation
  clears it.
- Presses on actionable targets keep the old mousedown-activates
  behavior (`isActionableHit_`), so links, form controls and `onclick`
  handlers behave exactly as before.
- The geometry lives in `layout/layout.cpp`: `hitTestText()` maps a
  point to (box, line, byte-offset-into-run-text) mirroring the
  renderer's line-origin and font-resolution math, `textBetween()`
  extracts document-order text, both unit-tested headlessly.

## 6. JavaScript API expansion (`js/jsbindings.cpp`)

- **`classList`** with `add/remove/toggle/contains/item/length/value`,
  backed by the class attribute and mutation-notifying like
  `className =` does.
- **Tree properties**: `firstChild`, `lastChild`, `nextSibling`,
  `previousSibling`, `nextElementSibling`, `previousElementSibling`,
  `nodeType`.
- **Tree methods**: `insertBefore`, `replaceChild`, `cloneNode(deep)`,
  `contains`, `closest(selector)`, `matches(selector)`.
- **Globals**: `btoa`/`atob`, strict `encodeURIComponent`/
  `decodeURIComponent` (space is `%20`, never `+` — unlike the
  form-style `urlEncode`), `requestAnimationFrame`/
  `cancelAnimationFrame` (a 16 ms timer through the regular timer
  machinery, so the event loop sleeps correctly), and a `navigator`
  object (`userAgent` kept in sync with the fetch UA string,
  `language`, `platform`).
- **`localStorage`** (persisted to `~/.minibrowser/local_storage`,
  atomic tmp+rename writes, `MB_STORAGE_FILE` overrides the path) and
  **`sessionStorage`** (memory-backed), both with the standard
  `getItem/setItem/removeItem/clear/key/length` surface.

## 7. Robustness / hygiene

- `fetchUrlCached` caps at 128 entries: every entry holds a full
  response body, and a long session could pin unbounded memory.
- Fixed a broken debug `fprintf` (unclosed quote) in the font loader.
- Fixed the `-Wmisleading-indentation` warning in the renderer.
- Build banner and user-agent bumped to 2.2; about:home documents the
  new features/shortcuts.

## 8. Testing

`tests/selftest.cpp` grew by 106 tests covering every item above:
parser implied end tags (including scope nesting), entity decoding,
`<base href>` extraction, classList, tree props/methods, base64/URI/
navigator/rAF/localStorage, and the selection geometry (hit-test snap
to codepoint boundaries, whole-line and cross-box extraction).
`round7.html` is a hand-checkable demo of the parser + JS additions.

---

# What changed in MiniBrowser 2.3 (round 4 of improvements)

This round adds the element-geometry API page scripts expect for
positioning, hit-testing and visual effects: `getBoundingClientRect()`
plus the `offset*` properties. The self-test suite grew from 413 to 452
assertions, all green, and `geometry.html` is a new hand-checkable demo
(outline boxes drawn by page JS at exactly the reported coordinates).

## 1. `Element.getBoundingClientRect()` (`js/jsbindings.cpp`)

Returns a DOMRect-shaped object (`x`, `y`, `width`, `height`, `top`,
`right`, `bottom`, `left`) with real browser semantics:

- **Viewport-relative coordinates** — `y` is document space minus the
  current scroll offset, evaluated at call time, so scrolling changes
  what page JS reads (exactly like the spec).
- **Union of fragments** — an element that wraps across lines returns
  the bounding box of all its fragments, not just one line's.
- **Zero rect, no throw** — detached elements (`createElement` without
  appending), `display:none` content and not-yet-laid-out nodes return
  an all-zero rect, matching what real browsers report for
  `display:none`. Calls never throw.

## 2. `offsetWidth` / `offsetHeight` / `offsetTop` / `offsetLeft`

Exposed as properties in the node get-trap. Sizes and position come
from the same fragment union; `offsetTop`/`offsetLeft` are
document-space (scroll-independent). In this engine there is a single
containing block, so document-space and offsetParent-relative coincide
for every practical page — documented in the binding.

## 3. Fragment-union geometry engine (`layout/layout.h`, `layout/layout.cpp`)

New `layout::rectForNode(lr, font, node, out)` computes the
DOCUMENT-space union of everything that belongs to a node:

- **Own box** — blocks, atomic images and form widgets match their box
  exactly (border-box).
- **Link fragments** — an `<a>` unions its per-line link hit-rects.
- **Inline run fragments** — `<b>`, `<i>`, `<span>` & co. own no box;
  their rects are the union of the runs they produced, measured with
  the exact geometry the renderer and `hitTestText` use (same font
  resolution, text-align handling, float insets, line heights and
  baseline placement for inline images). Runs from descendant inline
  elements match too (`<b><i>x</i></b>` unions both).

To make run ownership knowable, `Run` gained a `sourceNode` (the
element that produced the run):

- `collectRuns` tags text runs with the nearest element ancestor,
  `<img>`/`<br>` runs with the element itself, and block-in-inline
  break markers with the block child (so inline rects never grow to
  cover the child's box).
- The word-wrap engine now preserves `sourceNode` through word
  splitting, long-word breaking and the space-before-image run.
- **Merge guard**: adjacent runs only merge when produced by the SAME
  element. Previously same-styled runs from different elements merged
  into one run, which would attribute one element's fragment to
  another (and lose the second element's rect entirely).

## 4. JS-host geometry plumbing (`js/jsengine.h`, `app/browser.cpp`)

The JS layer holds no layout pointers. `JSEngine` gained two provider
callbacks wired once by `Browser` and read at call time:

- `rectForNode(node, out)` → `layout::rectForNode(layout_, font_, ...)`;
- `scrollY()` → current `scrollY_`.

Every relayout and scroll therefore automatically refreshes what page
JS sees, and a bare `JSEngine` without providers safely reports zeros.

## 5. Testing

`tests/selftest.cpp` grew by 7 tests / 39 assertions: block basics
(stacking, edge aliases, offset agreement), viewport-relativeness under
scroll (client top shifts by exactly the scroll amount while offsetTop
stays put), inline fragment nesting (`<i>` strictly inside `<b>` inside
`<p>`), link fragment rects, detached-element zero rects without
exceptions, the no-provider safety path, and a C++-level fragment-union
check. `geometry.html` draws outline boxes at the coordinates page JS
measured — the on-screen proof that outlines hug their targets.

---

# What changed in MiniBrowser 2.4 (round 9 of improvements)

Round 9 gives the browser its own media stack. Until now "video support"
meant forking an external mpv window; now `<video>` and `<audio>` decode
and play INSIDE the browser window — the way Firefox and Chrome do it —
and clicking a plain image opens an integrated zoom/pan viewer.

## 1. Internal media engine (`media/mediaplayer.{h,cpp}` — new)

- **Pipeline**: FFmpeg demux + decode (`avformat`/`avcodec`), video
  scaled to RGBA via `libswscale`, audio resampled to stereo S16 via
  `libswresample`, rendered as SDL streaming textures and played through
  the SDL audio device. One worker thread per player; the UI thread only
  copies the latest frame under a mutex and reads atomics — it never
  blocks on decode or I/O.
- **Wall-clock master clock**: video frames are published against the
  wall clock (paused players decode exactly one preview frame, like
  Chrome's poster frame). Audio queues best-effort on top; builds whose
  SDL has no audio (headless) still play video deterministically.
- **Transport**: play/pause/toggle, seek (with decoder flush), volume,
  mute, loop (restarts on EOF only when it was playing), autoplay that
  survives the open/decode race (`playWhenReady_` latch), end-of-stream
  parking with duration clamp.
- **Sources**: local paths, `file://`, plain `http://` (streamed natively
  by FFmpeg) and `https://` (streamed to a temp file by libcurl first —
  the bundled minimal FFmpeg build has no TLS). Everything is keyed by
  the RESOLVED URL, shared by layout, renderer, input and JS.
- **Registry**: one player per URL (`acquirePlayer`/`findPlayer`),
  pruned per navigation (`prunePlayersExcept`), torn down at exit.
- **Optional FFmpeg**: without the av* dev libraries the module compiles
  into a stub (players report "player unavailable", widgets show the
  error, mpv remains the playback path) — the rest of the browser is
  unaffected.

## 2. `<video>` / `<audio>` elements (`layout/`, `render/`)

- `layoutMedia()` sizes media widgets like images (CSS width/height →
  width/height attributes → tag defaults 320×180 / 360×36), resolves
  `src` / first `<source src>` / `poster`, and emits media boxes that
  flow through the normal block/widget placement (including
  `kWidgets`-last ordering inside containers).
- The renderer draws decoded frames through per-URL streaming textures
  (re-uploaded only when the frame sequence moves), the `poster` image
  until the first frame lands, a "Loading…"/error stage otherwise, and
  a Chrome-style control bar: play/pause glyph, seek track with red
  progress + knob, `0:01 / 0:08` time readout, mute toggle, volume
  slider. `<audio>` renders as the bar itself.
- **Shared control geometry**: `mediaGeom()` drives both drawing and
  `mediaHitTest()`, so what you can click is exactly what was drawn
  (play/seek/mute/volume/body — a click on the video body toggles
  playback, Chrome-style). Dragging on the seek bar scrubs.
- Attributes honored: `controls`, `autoplay`, `loop`, `muted`,
  `poster`, `width`, `height`. Relayouts re-apply them cheaply.

## 3. Built-in media pages + mpv demotion (`app/browser.cpp`)

- Navigating to a media URL/file (address bar, link, drag & drop) now
  shows a Chrome-style dark player page with the video playing in-page
  (autoplay + muted, mirroring Chrome's autoplay policy) and a
  "play in external mpv instead" escape link. mpv (and the `v` key /
  `mpv:` scheme) remain for yt-dlp-only targets such as YouTube watch
  pages; `loadHistoryEntry_` reloads media pages correctly.
- Repaint scheduling: playing videos throttle repaints to ~30 fps and
  the event loop wakes at 5 ms intervals; new-frame arrivals and
  seek/loop events trigger immediate repaints.

## 4. Integrated image viewer (`app/browser.cpp`, `render/`)

- Clicking a plain `<img>` (not inside a link, no onclick ancestor)
  opens a lightbox over the content area: fit-to-window at 100%, wheel
  zoom (5%–1200%), drag to pan, `+`/`-`/`0` and arrow keys, Esc/Enter or
  the × button closes, zoom% + hint line at the bottom.
- Detection uses `rectForNode` fragment rects, so it works for atomic
  image boxes AND bare `<img>` children that flow as inline runs inside
  a container's text box. Images with handlers or inside links keep the
  normal dispatch order (link navigation / onclick first).

## 5. JS media API (`js/jsbindings.cpp`, `js/jsengine.h`)

- HTMLMediaElement-lite on every node: `play()`, `pause()`, and the
  properties `paused`, `ended`, `duration`, `currentTime` (get/set —
  setting seeks), `muted` (get/set), `volume` (get/set), `loop` (set).
  Page JS and the engine agree on player identity through a new
  `resolveMedia` hook wired to the same URL resolution layout uses.

## 6. Build & tests

- `Makefile` links the FFmpeg libs via pkg-config when present
  (`-lpthread` always); the build stays green without them (stub path).
- `tests/selftest.cpp`: +7 media tests (URL heuristics, layout boxes,
  control hit-testing incl. a regression for the mute/volume dead-zone
  coordinates, open→decode→pixels→duration→play→pause→seek→EOF,
  audio-only playback, registry acquire/prune, JS play/pause/currentTime
  through the Duktape bindings) — suite now 520 assertions, all green
  with and without FFmpeg.
- New fixtures under `tests/media/` (generated): `sample.mp4`
  (h264+aac test pattern, 8 s), `tone.mp3` (5 s sine), `poster.png`.
  `media.html` is the interactive demo page.

# What changed in MiniBrowser 2.5 (round 10 of improvements)

Round 10 closes the gap that made YouTube unusable inside the browser:
`youtube.com/watch` links used to be handed to an external mpv process
(and silently did nothing when mpv — or yt-dlp for mpv's ytdl_hook — was
missing). The browser now resolves watch URLs itself through a yt-dlp
bridge and plays the result in the round-9 internal player, the same way
it plays every other media URL: in-process, in-window, no mpv.

## 1. yt-dlp bridge (`media/extractor.{h,cpp}` — new)

- **URL classification** (`isExtractableUrl`): watch-style links across
  all known host variants — `youtube.com/watch` (+ `www.`/`m.`/
  `music.`), `/shorts/`, `/embed/`, `/live/`, `/v/`, `youtu.be/<id>`
  short links and `youtube-nocookie.com/embed`. Home, search, channel
  and feed pages are NOT extractable (they keep loading the YouTube
  Lite page). `mpv:` links and `view-source:` are never auto-extracted.
- **Resolution** (`resolve`): shells out to `yt-dlp` (falls back to
  `youtube-dl`) with the format selector `b[protocol^=http]/b` — the
  best PROGRESSIVE format, i.e. video AND audio in one plain http(s)
  file, which is the only shape the one-input FFmpeg player can stream.
  Reality check: YouTube progressive tops out around 360p/720p; higher
  resolutions are DASH-only (separate audio/video tracks would need a
  two-input muxing engine).
- **Robustness**: URLs and arguments are single-quoted through
  `shellQuote` (proven by a popen round-trip test with quotes, `&`,
  `$()`, backticks); the tool's stderr lands in
  `$TMPDIR/mini-browser-ytdlp.log` and its last line is surfaced in the
  status bar when a resolve fails (bot checks, age gates...); a bot
  check additionally gets a cookie hint pointing at
  `MINIBROWSER_YTDLP_ARGS` (e.g. `--cookies-from-browser firefox`),
  appended verbatim to the resolver command line when set. Cold
  resolves are wrapped in `timeout 75` when coreutils' `timeout` exists.
- **Cache**: results are keyed by the original page URL with a 2 h TTL
  (googlevideo stream URLs live ~6 h), capped at 64 entries. Back/
  forward and reloads hit `peek()` — instant, no process, no network.
- **Threading**: `resolve()` is blocking and thread-safe; the browser
  calls it from a detached thread exactly like document fetches.

## 2. Browser integration (`app/browser.{h,cpp}`)

- `navigate()` routes extractable URLs through `openExtractedMedia_`
  (history keeps the original watch URL). `loadHistoryEntry_` does the
  same, so back/forward re-plays instantly from the extractor cache.
- **Async resolve**: `openExtractedMedia_` spawns the resolver on a
  detached thread; results land in a generation-keyed inbox
  (`BridgeSlot`) that `tick()` polls via `pollBridgeResult_` — an
  overlapping/superseded resolve can never clobber the current one
  (the old slot-overwrite race is designed out). `nextWakeupMs()`
  polls at 20 ms while a resolve is in flight.
- **Graceful degradation**: extraction failure (tool missing, bot
  check, private video...) falls back to the normal page load — the
  YouTube Lite page still renders, and the failure reason survives the
  page's own "HTTP 200" status update via `bridgeNote_`.
- **Media page, bridge form**: `loadMediaPage_(url, srcOverride,
  mediaTitle)` keeps the original watch URL for the address bar,
  history and the `mpv:` escape hatch, while `<video src>` points at
  the resolved stream; the caption shows the video title and a
  "stream resolved via yt-dlp" note. The `<title>` tag carries the
  video title, so the window title matches other browsers.

## 3. Bug fix found by the round-10 e2e: remote-https media never worked

`media/mediaplayer.cpp` downloaded https media to
`$TMPDIR/mini-browser-media/<hash>.bin` — but nothing ever created that
directory, so EVERY remote-https playback failed with "cannot create
temp file" (v2.4's selftests only exercised local files). The
downloader now mkdirs the cache directory and reports `errno` on
failure. Covered by a new env-gated live test (`MB_MEDIA_LIVE=1`)
that fetches a real https MP4 and waits for the first decoded frame.

## 4. Version & tests

- UA strings, banner and about:home bumped to 2.5 / round 10.
- `tests/selftest.cpp`: +4 test groups, suite now 555 assertions green
  (hermetic): extractor URL classification (21 cases), shellQuote incl.
  a real shell round-trip, cache/guard behavior, and the env-gated
  remote-https playback regression. Live extras: `MB_MEDIA_LIVE=1`
  (https playback), `MB_EXTRACTOR_LIVE=1` (real resolve sane-result
  check).

---

# What changed in MiniBrowser 2.6 (round 11 of improvements)

User report from 2.5: "in youtube it only shows the button and it always
fail in the bottom it says exiting mpv". Diagnosis: three stacked
failures, each invisible. (a) On machines without yt-dlp the bridge had
no in-browser playback path at all — it went straight to the YouTube
Lite page whose only button handed off to mpv. (b) The mpv hand-off
rendered unconditionally, even on machines without mpv, and clicking it
just printed "mpv exited: ..." in the status bar. (c) When the bridge
DID fail, the reason lived in the status bar only; the page looked like
a normal YouTube page with a dead button. Round 11 fixes all three, and
the headline fix removes the yt-dlp dependency entirely: a Piped API
fallback resolves streams server-side, so YouTube plays in the internal
player with NO local tooling and on bot-gated networks.

## 1. Piped fallback extractor (`media/extractor.{h,cpp}`)

- `resolve()` is now two-stage: yt-dlp subprocess first (best quality,
  unchanged), then a Piped API fallback — community extraction servers
  that resolve the video themselves and proxy the bytes, so a bot-gated
  or tool-less machine still plays. Verified end-to-end: a watch URL
  that yt-dlp cannot resolve ("Sign in to confirm you're not a bot")
  now decodes real frames in the internal player (screenshot
  `verification-youtube-internal-player-2.6.bmp`, caption reads
  "stream resolved via piped:pipedapi.ducks.party").
- Host list: `pipedapi.ducks.party`, `api.piped.private.coffee`
  (verified live 2026-10), overridable via
  `MINIBROWSER_PIPED_HOSTS=a.host,b.host` (comma/semicolon/space
  separated, trailing slashes trimmed). Per-host timeout 15 s; first
  success wins; failures compose into one error naming every host.
- Stream selection: progressive (video+audio in one file) only —
  `videoOnly==false`, non-HLS — preferring the proxied `MPEG_4` mp4
  (range requests verified, seek-friendly) over LBRY mirrors and webm,
  then by parsed quality ("720p" > "360p"). Titles come from the same
  JSON, so the player page shows the real video title even when the
  bridge path differs.
- `videoId()` pulls the 11-char id out of `?v=`, `/shorts/`, `/embed/`,
  `/live/`, `/v/` and `youtu.be` short links (5-16 chars of
  `[A-Za-z0-9_-]`), shared by the Piped URL builder.
- Error composition is honest and specific: video-gone (both paths
  report unavailable) vs bot-gated network (yt-dlp sign-in wall + piped
  network errors) vs missing tool — each with the actionable hint
  (cookies env var / install command). Failures are never cached, so a
  retry button re-runs the whole gauntlet.

## 2. Minimal JSON parser (`media/minjson.h` — new)

- Header-only, allocation-light, exception-free. Node-pool design:
  objects/arrays own contiguous child-index blocks (appended AFTER
  their subtree parses, so nested containers can never interleave a
  parent's child window), strings decode into one shared buffer with a
  NUL sentinel appended (every `str()` is NUL-terminated forever).
- Full RFC syntax: `\uXXXX` with surrogate pairs, strict numbers
  ("1." rejected), no trailing commas/comments. Malformed input fails
  `parse()` and every accessor on a wrong-typed/absent node returns a
  safe default — extractor call sites stay linear, no guards.

## 3. YouTube Lite page: failures speak up (`app/shims.cpp`)

- The page now takes the bridge failure reason and renders it as an
  amber banner: "Video playback failed: <reason>" (screenshot
  `verification-youtube-fallback-2.6.bmp`). The status bar keeps a
  capped copy too, but the page is where eyes are.
- New primary chip "▶ Play in browser" — links the watch URL, which
  re-runs the bridge (cache misses on failure, so it's a true retry).
- The "▶ Play in mpv" chip only renders when mpv is actually installed;
  otherwise the page says "mpv not installed — external playback
  disabled" instead of offering a button that prints "mpv exited".
- The media player page caption names the real resolver path
  ("stream resolved via piped:<host>" / "via yt-dlp") instead of
  hard-coding yt-dlp.

## 4. Browser plumbing (`app/browser.{h,cpp}`)

- The bridge failure reason travels through the async nav worker into
  `shims::youTubeLiteHtml` (new `bridgeReason` parameter on
  `loadRemotePage_`/`startAsyncFetch_`/`fetchDocumentWithShims`), so
  the banner appears on both the sync (screenshot) and interactive
  paths.
- `openExtractedMedia_` no longer bails to the Lite page when yt-dlp is
  missing — resolve() handles that via Piped. Status lines updated:
  "Resolving video stream (yt-dlp → Piped fallback)…"; the mpv probe
  message now reads "external player failed — <log tail>".
- `BridgeResult`/`loadMediaPage_` carry `via` for the caption.

## 5. Version & tests

- UA strings, banner and about:home bumped to 2.6 / round 11.
- `tests/selftest.cpp`: +73 assertions — video id extraction (15 URL
  shapes), minjson (payload walk, escapes, surrogate-safe UTF-8, 11
  malformed inputs, wrong-type/OOB accessor safety), Piped stream
  selection (distractors: video-only DASH, HLS, LBRY; error payload;
  garbage), host-list parsing incl. env override. Suite: 628 green.
  Two of the new tests caught real bugs during development (dangling
  `Val*` in the stream picker; kids-window interleaving in the JSON
  parser).
- Screenshot tool waits up to 9 s for the first decoded frame —
  bridged YouTube spends its first seconds resolving, so 4 s missed
  the frame.

---

# What changed in MiniBrowser 2.7

This round drops the "YouTube Lite" and "DuckDuckGo Lite" shims entirely
and instead makes the browser pretend to be a real Chrome on Linux so the
actual services stop treating it as a bot.

## 1. Real Chrome User-Agent + Sec-Fetch-* headers (`net/fetch.cpp`)

- The User-Agent is now the real Chrome-on-Linux string:
  `Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko)
  Chrome/120.0.0.0 Safari/537.36`.
- The request now sends the full Chrome navigation header bundle:
  `Accept: text/html,application/xhtml+xml,...`, `Accept-Language`,
  `Upgrade-Insecure-Requests: 1`, `Sec-CH-UA`, `Sec-CH-UA-Mobile`,
  `Sec-CH-UA-Platform`, `Sec-Fetch-Dest: document`,
  `Sec-Fetch-Mode: navigate`, `Sec-Fetch-Site: none`,
  `Sec-Fetch-User: ?1`.
- YouTube's EU consent cookies (`SOCS`/`CONSENT`/`PREF`) are kept — they
  are not a shim, they encode the consent choice a real Chrome session
  on an EU IP would also persist.
- Net effect: YouTube ships the real ~900KB watch / search / home HTML;
  Google ships its real search page; DuckDuckGo ships its real SPA shell.

## 2. Removed: DuckDuckGo Lite rewrite (`app/shims.{h,cpp}`)

- `maybeRewriteSearchUrl` now returns `""` (no rewrite).
  `google.com/search?q=...` is fetched as-is; the Chrome UA + Sec-Fetch-*
  headers make Google serve its real search page.
- `webSearchUrl` (used by the address bar for non-URL inputs) now points
  at the real DuckDuckGo, `https://duckduckgo.com/?q=...` — no more
  `lite.duckduckgo.com/lite`.
- The previous `extractAssignedObject` / `walkJson` / `videoListHtml`
  / `kSearchForm` helpers in `app/shims.cpp` are removed (the file is
  now ~120 lines vs ~490 before).

## 3. Removed: YouTube Lite page (`app/shims.{h,cpp}`)

- `youTubeLiteHtml` is now a no-op: it returns `""` with `ok=false`,
  which tells `fetchDocumentWithShims` in `app/browser.cpp` to keep the
  original page body and render it as-is.
- The amber "Video playback failed:" banner is gone with it; the
  failure reason now lives in `pageInfo_` (the status bar) which the
  user can still read.
- The `bridgeReason` parameter is kept on the signature so the existing
  call sites in `app/browser.cpp` and `tests/selftest.cpp` compile
  unchanged — but it is no longer consumed.

## 4. Tests (`tests/selftest.cpp`)

- `youtube_shim_never_raw_html` is rewritten to assert the new contract:
  every call to `youTubeLiteHtml` returns `""` + `ok=false` regardless
  of the page kind (watch / search) and regardless of whether a bridge
  reason was supplied.
- New assertions: `isYouTubeUrl` host classification; `webSearchUrl`
  targets `duckduckgo.com` and never `lite.duckduckgo.com`;
  `maybeRewriteSearchUrl` returns `""` for `google.com/search`,
  `duckduckgo.com`, and any non-google URL.
- Suite: 631 green (was 628 in 2.6 — the new assertions are net new).

## 5. About page

- `about:home` bumped to 2.7 / round 12; description and the "Try it
  out" link list now reflect the shim-less behaviour (links to real
  YouTube home and real Google search added so a curious user can
  confirm the change directly).

---

# What changed in MiniBrowser 2.8

v2.7 removed the YouTube / DuckDuckGo lite shims and switched to a real
Chrome User-Agent + Sec-Fetch-* headers, on the assumption that the
Chrome UA alone would make the actual services render. It didn't:
YouTube / Google / DuckDuckGo are all JavaScript SPAs, and the engine's
Duktape ES5.1 interpreter cannot run their modern ES6+ code, so the SPA
shells loaded but rendered blank (YouTube home: 887 KB HTML, 857 KB of
`<script>`, only ~163 chars of visible text — just the footer).

v2.8 is hybrid: the Chrome UA stays (so sites ship the real HTML, with
the ytInitialData JSON reliably present on YouTube), AND a minimal
data-extractor is reintroduced to render the actual page's data
statically.

## 1. YouTube data-extractor REINTRODUCED (`app/shims.cpp`)

- `youTubeLiteHtml` is back, but the page is branded just "YouTube"
  (NOT "YouTube Lite" — it is the actual page's own ytInitialData /
  ytInitialPlayerResponse JSON, rendered statically because the JS
  engine cannot drive the SPA). The v2.6 `kSearchForm` had
  `<b>YouTube Lite</b>`; v2.8 dropped "Lite".
- With the Chrome UA in `net/fetch.cpp`, the JSON is reliably present
  in the page. In v2.6 the old "MiniBrowser" UA made YouTube serve a
  bot/consent stub that often had no JSON — the extractor was flaky.
  Now it works every time.
- `fetchDocumentWithShims` in `app/browser.cpp` now runs the extractor
  EVEN on fetch failures (`fr.ok=false`). YouTube serves a 429 +
  redirect to `google.com/sorry/` to datacenter IPs (this sandbox is
  one); the v2.6/v2.7 code never entered the extractor on that path
  — the user saw a network-error page instead of the YouTube fallback
  page. The extractor's watch-page branch only needs the videoId from
  the URL, so it renders a playable page (Play-in-browser chip +
  "metadata could not be read" message) even on a 429 /sorry/ body.
- Live verification (this sandbox, datacenter IP):
  - YouTube search `?search_query=hello`: 12 KB extracted HTML,
    39 layout boxes, thumbnails + titles + channels + view counts
    (was 6 stub boxes in v2.7).
  - YouTube watch `?v=dQw4w9WgXcQ`: Piped API resolves the stream
    URL, `loadMediaPage_` renders the in-browser media player
    (5 boxes; the extractor's watch fallback is only used when
    Piped AND yt-dlp both fail).
  - YouTube home: extractor runs, finds no videos in ytInitialData
    (home page is consent-gated even with the SOCS/CONSENT cookies),
    falls back to the search-box + category-shortcuts page (1 KB,
    6 boxes).

## 2. Address-bar search switched to real DuckDuckGo HTML endpoint

- `shims::webSearchUrl` now returns
  `https://html.duckduckgo.com/html/?q=...` (the actual server-rendered
  DuckDuckGo endpoint, NOT `lite.duckduckgo.com/lite` — that was the
  "lite" alternative the user explicitly asked to remove in v2.7).
  `html.duckduckgo.com/html` is the same DuckDuckGo service, just
  server-rendered HTML, no JS required, so the engine renders the
  results list directly.
- Live verification: typing `hello world` in the address bar now
  produces 41 layout boxes (was 5 stub boxes in v2.7) at 98%
  non-white pixels — real search results with titles + snippets +
  URLs. DDG HTML endpoint serves 35 KB HTML with 4.7 KB of visible
  text per query.

## 3. Google search stays as pass-through

- `maybeRewriteSearchUrl` still returns `""` (no rewrite). The user
  explicitly asked NOT to rewrite `google.com/search`. The Chrome UA
  makes Google serve its real search page; that page is a JS SPA so
  it still renders mostly blank in our engine, but that's the user's
  explicit choice. Address-bar search phrases (the other entry point)
  go to `html.duckduckgo.com/html` which DOES render.

## 4. Tests (`tests/selftest.cpp`)

- `youtube_shim_never_raw_html` updated to assert the v2.8 contract:
  watch page returns HTML with `<b>YouTube</b>` header (NOT
  `YouTube Lite`), `Play in browser` chip, `mpv:` link when mpv
  available, search page returns the form. The bridge-failure banner
  (`Video playback failed: <reason>`) is asserted.
- New assertions: `webSearchUrl` targets `html.duckduckgo.com/html`
  and never `lite.duckduckgo.com` or `duckduckgo.com/lite`;
  `maybeRewriteSearchUrl` returns `""` for `google.com/search`,
  `duckduckgo.com`, and any non-google URL.
- Suite: 644 green (was 631 in 2.7).

## 5. About page

- `about:home` bumped to 2.8 / round 13; description now reflects
  the hybrid approach (Chrome UA + YouTube data-extractor + DDG html
  endpoint); "Try it out" link list updated with the new behaviour.

---

# What changed in MiniBrowser 2.9

v2.8 made the browser pretend to be Chrome (UA + Sec-Fetch-* + Sec-CH-UA
basics) and reintroduced the YouTube ytInitialData data-extractor + the
real server-rendered DDG html endpoint. v2.9 makes the impersonation
deeper — the full Chrome 120 client-hints bundle, HTTP/2, persistent
cookie jar, Referer tracking, and Expect header suppression.

Investigation: even with the v2.8 Chrome UA + Sec-Fetch-* headers, YouTube
served a 429 + redirect to `google.com/sorry/` on watch URLs from
datacenter IPs. Adding the full client-hints bundle + curl-impersonate
(which matches Chrome's BoringSSL TLS fingerprint exactly) STILL gets
429'd — the block is at the IP level, not the TLS/HTTP fingerprint
level. From a residential IP, the v2.8 code already works for search
(`videoId count: 365` in the search JSON) and Piped works for watch
playback. v2.9's improvements help on residential IPs and at the
margins of bot detection.

## 1. Full Chrome 120 client-hints bundle (`net/fetch.cpp`)

- v2.8 sent Sec-CH-UA, Sec-CH-UA-Mobile, Sec-CH-UA-Platform.
- v2.9 adds the full set Chrome 120 sends on every navigation:
  `Sec-CH-UA-Full-Version-List` (with full version of every brand, not
  just the major — Google uses this to distinguish real Chrome from the
  spoofable basic Sec-CH-UA), `Sec-CH-UA-Full-Version`,
  `Sec-CH-UA-Arch: "x86"`, `Sec-CH-UA-Bitness: "64"`,
  `Sec-CH-UA-Model: ""`, `Sec-CH-UA-Platform-Version: "6.5.0"`,
  `Sec-CH-UA-Form-Factors: "Desktop"`, `Sec-CH-UA-WoW64: ?0`,
  `Sec-CH-DPR: 1`, `Sec-CH-Viewport-Width: 1024`, `Sec-CH-Width: 1024`,
  `Device-Memory: 8`, `X-Client-Data: CIm2yQEIpt3JAULckK8FBLjTzAEIzufA`
  (Chrome-specific opaque base64 blob; sites check for its presence more
  than its specific content), `Priority: u=0, i` (HTTP/2 priority hint),
  and `Expect:` (empty, suppresses the libcurl default
  `Expect: 100-continue` that real Chrome never sends on GETs).

## 2. HTTP/2 (`net/fetch.cpp`)

- `CURLOPT_HTTP_VERSION = CURL_HTTP_VERSION_2_0`. Chrome uses h2 by
  default for HTTPS; some sites (notably Google properties) treat
  HTTP/1.1 requests as a strong bot signal because no real Chrome user
  is on HTTP/1.1 anymore. libcurl supports h2 with the system nghttp2.

## 3. Persistent cookie jar (`net/fetch.cpp`)

- New `cookieJarPath()` returns `$HOME/.cache/minibrowser/cookies.txt`
  (the parent dir is created on first call via `mkdir -p`).
- `CURLOPT_COOKIEFILE` reads cookies from the jar before each request;
  `CURLOPT_COOKIEJAR` declares the file as the write target.
- Critical bug fix: libcurl's `CURLOPT_COOKIEJAR` only writes the file
  on `curl_easy_cleanup`, but our per-thread handle is REUSED across
  requests (for the connection cache) and NEVER cleaned up. The
  `CURLOPT_COOKIELIST = "FLUSHALL"` command word is documented as
  flushing immediately, but in practice it only clears the
  "new cookies" flag — the actual file write still requires cleanup.
  Verified by a minimal libcurl test (cookie_test2.cpp): without
  cleanup, the jar file is NOT created even after FLUSHALL.
- Fix: extract the in-memory cookies via
  `curl_easy_getinfo(curl, CURLINFO_COOKIELIST, &cookies)` (each entry
  comes back in Netscape cookie file format already) and write them to
  the jar file ourselves after each `curl_easy_perform`. The handle can
  stay reused for the connection cache, and YouTube's session cookies
  (VISITOR_INFO1_LIVE, __Secure-YNID, GPS, YSC, __Secure-ROLLOUT_TOKEN)
  now persist across requests AND across process restarts.
- Live verification: `cookies.txt` is written after the first YouTube
  fetch, containing all 7 of YouTube's session cookies. Subsequent
  fetches read them back via `CURLOPT_COOKIEFILE`.

## 4. Referer header tracking (`net/fetch.{h,cpp}`)

- New `setReferer(const std::string& url)` exported in fetch.h.
  Callers can set the Referer for the next fetchUrl() call to the URL
  of the page the user is currently on. When set, the request sends
  `Referer: <url>` and `Sec-Fetch-Site: same-origin` (instead of
  `Sec-Fetch-Site: none` for top-level typed URLs). Mimics Chrome's
  exact behaviour for in-site navigations.

## 5. About page

- `about:home` bumped to 2.9 / round 14. Description now lists the
  full client-hints bundle, HTTP/2, cookie jar, and Referer tracking.
  "Try it out" link list updated with explicit YouTube search / watch
  links. Tip mentions the cookie jar file path so the user knows where
  to delete it to clear their session.

## 6. Tests

- All 644 selftests still pass (no new tests needed for v2.9 since the
  changes are at the network layer; the v2.8 assertions for the
  YouTube extractor and the DDG html endpoint are unchanged).

## 7. What's NOT here

- libcurl-impersonate integration: the library crashes (segfault) on
  every `curl_easy_impersonate()` call when linked directly into our
  binary, regardless of the target value (1-15) or whether curl_global_init
  is called first. Likely a binary incompatibility with our libstdc++
  / glibc combo that I couldn't debug in this sandbox. Even when working
  (via the curl-impersonate-chrome binary), YouTube STILL serves 429
  from datacenter IPs — confirming the block is IP-level, not
  TLS-fingerprint-level. From residential IPs the v2.8 code already
  works for search; v2.9's improvements help at the margins.

---

# What changed in MiniBrowser 2.10

## Background: the YouTube watch playback issue you reported

You reported that "Never Gonna Give You Up" (`dQw4w9WgXcQ`) plays fine,
but other videos show "Video playback failed: video unavailable on
YouTube (removed, private or region-locked) — confirmed by both yt-dlp
and Piped". Investigation with the actual video IDs from your
screenshots revealed two distinct failure modes that v2.9 was
conflating into one misleading "unavailable" message:

  1. **Genuinely unavailable videos** (e.g. `s35dFYtR0B4`,
     `s3SdFYTRoB4` — IDs that don't exist on YouTube anymore).
     YouTube's oembed returns 404; Piped returns
     `ContentNotAvailableException: This video is unavailable`. The
     browser's existing "video unavailable" message was correct.

  2. **Piped-bot-flagged videos** (e.g. `7FwDP17XPlk` — a "Rick Astley
     - Never Gonna Give You Up" re-upload by "Amazing Lyrics"). The
     video IS available (oembed returns 200 with full metadata), but
     Piped returns `SignInConfirmNotBotException: YouTube probably
     temporarily blocked anonymous watch access with this IP, got error
     LOGIN_REQUIRED: "Sign in to confirm you're not a bot"`. This
     affects ALL Piped instances globally (verified: tested 17 public
     instances — only `pipedapi.ducks.party` and
     `api.piped.private.coffee` are reachable, both return the same
     `SignInConfirmNotBotException`). YouTube started requiring sign-in
     for anonymous watch access on the NewPipeExtractor client that
     Piped uses. Piped is now effectively dead for YouTube watch URLs
     in 2026 — the bot block is at the YouTube-API level, not the
     Piped-instance-IP level.

v2.9's `unavailable` check matched the first error pattern only, so
the second failure mode was incorrectly bucketed as "yt-dlp not
installed" or "Piped failed" — but never as the bot-gate it actually
is. v2.10 fixes the diagnostics and adds the two workarounds that
actually unblock playback.

## 1. Distinguish "video unavailable" from "Piped got bot-flagged"
   (`media/extractor.cpp`)

The error composition now checks BOTH `ytdlpErr` AND `pipedErr` for
bot-gate signals (was only checking `ytdlpErr`, which is empty when
yt-dlp isn't installed). Bot-gate signals matched:
`SignInConfirm`, `LOGIN_REQUIRED`, `Sign in to confirm`,
`anonymous watch access`. When detected, the failure message now
reads:

> YouTube is bot-gating Piped: <full Piped error>. The video itself
> is fine — Piped's IP got flagged. Install yt-dlp (pip install
> yt-dlp) — its rotating clients and cookie support usually bypass
> this; or set MINIBROWSER_YTDLP_ARGS='--cookies-from-browser firefox'
> to reuse your browser's YouTube session

The `unavailable` message is preserved (and clarified to include
"age-restricted" — age-restricted videos report `unavailable` via
Piped but are technically available to signed-in users).

## 2. yt-dlp auto-passes `--cookies-from-browser` + Android client
   (`media/extractor.cpp`)

When yt-dlp is installed, the bridge now adds:

  - `--extractor-args youtube:player_client=android,tv_embedded,web`
    — forces the bot-tolerant Android client first (uses a different
    YouTube API path that's still bot-tolerant), then the
    age-gate-bypassable `tv_embedded` client, then the default `web`
    client as last resort.
  - `--cookies-from-browser firefox` (or `chrome` / `brave` / `chromium`
    / `edge`) — auto-detected by checking `$HOME/.mozilla/firefox`,
    `$HOME/.config/google-chrome`, etc. Reuses the user's signed-in
    YouTube session, which bypasses YouTube's "Sign in to confirm
    you're not a bot" wall entirely.

The user can still override via `MINIBROWSER_YTDLP_ARGS=...` (including
setting it to empty to disable the auto-detection).

## 3. About page

`about:home` bumped to 2.10 / round 15. The description now
explicitly recommends installing yt-dlp as the reliable YouTube
playback path in 2026, with Piped as a fallback only for videos that
yt-dlp can't reach.

## What's NOT here

I considered but didn't add:
- **More Piped instances** to the host list — tested 17 public
  instances, only 2 are reachable (`pipedapi.ducks.party` and
  `api.piped.private.coffee`), and BOTH return the same
  `SignInConfirmNotBotException`. Adding more wouldn't help — the bot
  block is at the YouTube-API level, not the instance-IP level.
- **Invidious as a third fallback** — only 1 of 11 public Invidious
  instances has the API enabled (`invidious.f5.si`), and it returns an
  HTML error page ("Oh noes!") instead of JSON. Invidious is also
  effectively dead for stream extraction in 2026.
- **libcurl-impersonate integration** — the library still segfaults
  on every `curl_easy_impersonate()` call when linked into our
  binary. And even if it worked, it doesn't help — the bot block is
  at the YouTube-API level (requiring sign-in for the NewPipeExtractor
  client), not the TLS-fingerprint level.

The realistic conclusion: in 2026, YouTube watch playback requires
either (a) yt-dlp with cookies from a signed-in browser session, or
(b) the official YouTube Data API v3 with an API key. Piped/Invidious
are no longer viable. v2.10 makes path (a) automatic when yt-dlp is
installed.

---

# What changed in MiniBrowser 2.11

## Background: the buzz sound you reported

You reported that YouTube watch playback now WORKS (the video plays) but
the audio is "just buzz sound, not the actual content of the video".
Investigation found two distinct root causes — both contributed:

  1. **yt-dlp format selector was matching DASH protocols too.** The
     v2.10 selector `b[protocol^=http]/b` was supposed to prefer
     progressive (combined video+audio) formats over DASH (separate
     video-only / audio-only) streams. But `protocol^=http` matches
     BOTH `https` (progressive) AND `http_dash_segments` (DASH). For
     modern YouTube videos that only have DASH adaptive streams (most
     high-quality uploads in 2026), the selector would fall through to
     the bare `b` fallback, which picks the best video-only DASH
     stream. Result: a file with video but NO audio track. The player
     would open it, find no audio stream, and the SDL audio device
     would play silence (which on some systems sounds like a low
     buzz/hum from the speaker driver).

  2. **swresample was initialized from incomplete codec context data.**
     The v2.10 code set up swr at `open()` time using
     `actx_->ch_layout` / `actx_->sample_fmt` / `actx_->sample_rate`.
     For AAC streams in particular, FFmpeg only fully populates these
     fields AFTER the first `avcodec_receive_frame` call — at open
     time, `ch_layout` can be `{0}` (unspecified), and swr would
     silently produce garbage output (the buzz symptom). Even when
     the file HAD audio (rare progressive streams), the audio path
     was broken for AAC.

## 1. yt-dlp format selector now requires both audio AND video codecs
   (`media/extractor.cpp`)

The v2.10 selector `b[protocol^=http]/b` is replaced with the
explicit four-step selector:

    best[protocol=https][acodec!=none][vcodec!=none]
    /best[acodec!=none][vcodec!=none]
    /best[acodec!=none]
    /best

This:
- Step 1: prefer progressive `https` combined formats (the ideal case)
- Step 2: accept any combined format (audio+video in one file)
- Step 3: as last resort, accept any format with audio (rare — only
  if no combined formats exist at all, would be audio-only)
- Step 4: absolute last resort, best anything (might be video-only —
  no audio, but at least the user sees the video)

The v2.10 fallback `/b` was too permissive and would pick a
video-only DASH stream when no progressive format existed.

## 2. swresample is now initialized LAZILY on the first audio frame
   (`media/mediaplayer.cpp`)

`swr_alloc_set_opts2` is now called from inside `decodeAudioPacket_`
on the FIRST `avcodec_receive_frame` output, using the frame's
actual `ch_layout` / `format` / `sample_rate` (which FFmpeg has
fully populated by then). The v2.10 code called it at `open()` time
from the codec context's `ch_layout` etc., which can be `{0}` /
`AV_SAMPLE_FMT_NONE` / 0 for AAC streams until the first decode —
causing swr to silently emit garbage (the buzz).

The new code:
- Logs `[audio] swr initialized: in=44100Hz fltp 1ch -> out=48000Hz
  S16 stereo` on success (so the user can verify the audio path
  took the right format from the actual decoded stream).
- Logs `[audio] swr init FAILED (in=... rc=...) — audio will be
  silent` on failure (with the FFmpeg error code for debugging).
- Falls back to `ctx->ch_layout` etc. if the frame's fields are
  unset (defensive — shouldn't happen in practice, but covers
  edge cases).
- Guards against `inRate <= 0` (avoids division by zero in the
  `outMax` computation).

## 3. SDL audio device now allows frequency + samples negotiation
   (`media/mediaplayer.cpp`)

`SDL_OpenAudioDevice` is now called with
`SDL_AUDIO_ALLOW_FREQUENCY_CHANGE | SDL_AUDIO_ALLOW_SAMPLES_CHANGE`
instead of `0`. If the system audio device can't honor 48kHz / S16 /
stereo / 2048-sample buffers exactly (e.g., it's locked at 44.1kHz
because another app is using it), SDL can now negotiate the closest
match. swr resamples from the source's actual rate (44.1kHz for AAC)
to whatever the device settled on (`audioRate_` reflects the real
`have.freq`). The v2.10 code with `0` would have failed to open the
device entirely on systems that can't match 48kHz exactly — leading
to no audio at all.

## 4. About page

`about:home` bumped to 2.11 / round 16. Description highlights both
fixes (yt-dlp format selector + lazy swr init).

## Verification

- Build clean, 644 selftests pass (unchanged from v2.10 — the
  selftest doesn't exercise the audio device path because there's
  no audio device in the sandbox).
- Live test with `tests/media/sample.mp4` (which has 44.1kHz mono
  AAC audio): `[audio] swr initialized: in=44100Hz fltp 1ch ->
  out=48000Hz S16 stereo` — the lazy init correctly identified the
  actual decoded format and set up the converter with the right
  parameters. The v2.10 code would have used `actx_->ch_layout`
  (which is `{0}` for this codec at open time) and produced
  garbage.

---

# What changed in MiniBrowser 2.12

## Background: the buzz sound still present after v2.11

You reported that after v2.11, the buzz sound is "still no sound
accept that quite buzz" — quieter than v2.10's buzz, but still a buzz,
not actual audio. v2.11 made the buzz quieter because the lazy swr
init succeeded (was producing loud garbage before, now produces
silence/proper audio). The remaining buzz suggests either:
  1. The audio device is open but receiving silence (no PCM produced) —
     speaker driver noise on idle.
  2. swr is producing wrong output despite being set up correctly.
  3. The source file has no audio track at all (yt-dlp picked a
     video-only DASH stream — hasAudio_ is false, no audio device is
     opened, but the system might still produce a low hum).

## 1. Switched audio output format from S16 to F32 (`media/mediaplayer.cpp`)

The SDL audio device now opens with `AUDIO_F32SYS` instead of
`AUDIO_S16SYS`. FFmpeg's AAC/Opus decoders produce float planar
(FLTP) natively; the v2.11 path converted FLTP -> S16 which adds
a quantization step. If swr was misconfigured (e.g., wrong input
channel layout), the S16 quantization could produce a quiet buzz
instead of proper audio. F32 output lets swr convert FLTP -> FLT
(packed float) which is a near-identity transform (just
de-planarization) — much less likely to produce artifacts. SDL2
supports AUDIO_F32SYS natively on all modern audio backends
(PulseAudio, PipeWire, ALSA, CoreAudio, WASAPI).

The audio callback (`audioCallbackC`) now handles both F32 and S16
device formats. Volume scaling uses the right type (float for F32,
int16 for S16). The PCM ring buffer is now `std::vector<uint8_t>`
(bytes) instead of `std::vector<int16_t>` to support either format.

New member `outBytesPerSample_` (4 for F32, 2 for S16) is set by the
swr setup and used by the callback for frame alignment.

## 2. Comprehensive diagnostic logging (`media/mediaplayer.cpp` + `media/extractor.cpp`)

To diagnose the remaining buzz, v2.12 now logs:

  - `[media] open: <path>` — what file/URL the player is opening
  - `[media]   vStream=N aStream=N duration=Ns` — whether video and
    audio streams were found, and the duration
  - `[audio] NO AUDIO STREAM in file — this is a video-only DASH
    stream. Audio will be silent.` — printed when `aStream < 0`. This
    is the smoking gun: if yt-dlp picked a video-only DASH stream (the
    v2.11 fallback `best` prefers video-only for high-quality YouTube
    uploads), the file has no audio track. Audio will be silent.
  - `[audio] stream found: codec=aac sample_rate=44100 channels=1
    sample_fmt=8` — the audio track's actual format (codec, rate,
    channels, FFmpeg sample_fmt number)
  - `[audio] device opened: freq=48000 fmt=F32 ch=2 samples=2048` —
    the SDL audio device's actual negotiated format
  - `[audio] SDL_OpenAudioDevice FAILED: <error> — audio will be
    silent` — when the device can't be opened
  - `[audio] swr initialized: in=44100Hz fltp 1ch -> out=48000Hz flt
    stereo (device fmt=F32)` — the swr conversion setup
  - `[audio] swr init FAILED (in=... rc=...) — audio will be silent`
    — when swr can't be initialized
  - `[extractor] yt-dlp returned N line(s), rc=0 — first: <url> —
    second: <title>` — what yt-dlp returned (URL + title + ext)

The user can run `./browser <url> 2>log.txt` and share the log to
diagnose the exact failure mode.

## 3. yt-dlp format selector unchanged from v2.11

The v2.11 four-step selector is kept:
    best[protocol=https][acodec!=none][vcodec!=none]
    /best[acodec!=none][vcodec!=none]
    /best[acodec!=none]
    /best

If the diagnostic logs show `[audio] NO AUDIO STREAM in file`, the
fix is to either (a) install yt-dlp with a newer version that
supports merging DASH streams, or (b) add explicit format-id
fallbacks like `18/22` (the legacy progressive mp4 formats) to the
selector. We'll decide based on the logs.

## Verification

- Build clean, 644 selftests pass.
- Live test with `tests/media/sample.mp4` (44.1kHz mono AAC):
  ```
  [media] open: tests/media/sample.mp4
  [media]   vStream=0 aStream=1 duration=8s
  [audio] stream found: codec=aac sample_rate=44100 channels=1 sample_fmt=8
  [audio] SDL_OpenAudioDevice FAILED: ALSA: Couldn't open audio device — audio will be silent
  [audio] swr initialized: in=44100Hz fltp 1ch -> out=48000Hz flt stereo (device fmt=0)
  ```
  (sandbox has no audio device — expected. The user's machine will
  show `device fmt=F32` instead of `device fmt=0`.)
