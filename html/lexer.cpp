#include "lexer.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

namespace browser {

// Named HTML entities. A table (not an if-chain) because real pages use
// far more than the original handful — arrows, typographic quotes and
// currency signs appear constantly on Wikipedia/blog content. Sorted by
// name so lookup is a binary search; all names are lowercase (HTML
// entity matching is case-sensitive, but the table holds the canonical
// spelling — the lookup below is exact).
struct NamedEntity { const char* name; const char* utf8; };
static const NamedEntity kEntities[] = {
    {"Aacute", "\xc3\x81"}, {"aacute", "\xc3\xa1"},
    {"Acirc", "\xc3\x82"}, {"acirc", "\xc3\xa2"},
    {"acute", "\xc2\xb4"},
    {"AElig", "\xc3\x86"}, {"aelig", "\xc3\xa6"},
    {"Agrave", "\xc3\x80"}, {"agrave", "\xc3\xa0"},
    {"Alpha", "\xce\x91"}, {"alpha", "\xce\xb1"},
    {"amp", "&"},
    {"and", "\xe2\x88\xa7"},
    {"ang", "\xe2\x88\xa0"},
    {"apos", "'"},
    {"auml", "\xc3\xa4"}, {"Auml", "\xc3\x84"},
    {"beta", "\xce\xb2"}, {"Beta", "\xce\x92"},
    {"brvbar", "\xc2\xa6"},
    {"bull", "\xe2\x80\xa2"},
    {"Ccedil", "\xc3\x87"}, {"ccedil", "\xc3\xa7"},
    {"cedil", "\xc2\xb8"},
    {"cent", "\xc2\xa2"},
    {"check", "\xe2\x9c\x93"},
    {"Chi", "\xce\xa7"}, {"chi", "\xcf\x87"},
    {"circ", "\xcb\x86"},
    {"clubs", "\xe2\x99\xa3"},
    {"cong", "\xe2\x89\x85"},
    {"copy", "\xc2\xa9"},
    {"crarr", "\xe2\x86\xb5"},
    {"cup", "\xe2\x88\xaa"},
    {"curren", "\xc2\xa4"},
    {"dagger", "\xe2\x80\xa0"}, {"Dagger", "\xe2\x80\xa1"},
    {"darr", "\xe2\x86\x93"}, {"dArr", "\xe2\x87\x93"},
    {"deg", "\xc2\xb0"},
    {"Delta", "\xce\x94"}, {"delta", "\xce\xb4"},
    {"diams", "\xe2\x99\xa6"},
    {"divide", "\xc3\xb7"},
    {"Eacute", "\xc3\x89"}, {"eacute", "\xc3\xa9"},
    {"Ecirc", "\xc3\x8a"}, {"ecirc", "\xc3\xaa"},
    {"Egrave", "\xc3\x88"}, {"egrave", "\xc3\xa8"},
    {"empty", "\xe2\x88\x85"},
    {"epsilon", "\xce\xb5"}, {"Epsilon", "\xce\x95"},
    {"equiv", "\xe2\x89\xa1"},
    {"eta", "\xce\xb7"}, {"Eta", "\xce\x97"},
    {"ETH", "\xc3\x90"}, {"eth", "\xc3\xb0"},
    {"Euml", "\xc3\x8b"}, {"euml", "\xc3\xab"},
    {"euro", "\xe2\x82\xac"},
    {"exist", "\xe2\x88\x83"},
    {"fnof", "\xc6\x92"},
    {"forall", "\xe2\x88\x80"},
    {"frac12", "\xc2\xbd"}, {"frac14", "\xc2\xbc"}, {"frac34", "\xc2\xbe"},
    {"Gamma", "\xce\x93"}, {"gamma", "\xce\xb3"},
    {"ge", "\xe2\x89\xa5"},
    {"gt", ">"},
    {"harr", "\xe2\x86\x94"}, {"hArr", "\xe2\x87\x94"},
    {"hearts", "\xe2\x99\xa5"},
    {"hellip", "\xe2\x80\xa6"},
    {"Iacute", "\xc3\x8d"}, {"iacute", "\xc3\xad"},
    {"Icirc", "\xc3\x8e"}, {"icirc", "\xc3\xae"},
    {"iexcl", "\xc2\xa1"},
    {"Igrave", "\xc3\x8c"}, {"igrave", "\xc3\xac"},
    {"image", "\xe2\x84\x91"},
    {"infin", "\xe2\x88\x9e"},
    {"int", "\xe2\x88\xab"},
    {"Iota", "\xce\x99"}, {"iota", "\xce\xb9"},
    {"iquest", "\xc2\xbf"},
    {"isin", "\xe2\x88\x88"},
    {"Iuml", "\xc3\x8f"}, {"iuml", "\xc3\xaf"},
    {"kappa", "\xce\xba"}, {"Kappa", "\xce\x9a"},
    {"Lambda", "\xce\x9b"}, {"lambda", "\xce\xbb"},
    {"laquo", "\xc2\xab"},
    {"larr", "\xe2\x86\x90"}, {"lArr", "\xe2\x87\x90"},
    {"lceil", "\xe2\x8c\x88"},
    {"ldquo", "\xe2\x80\x9c"},
    {"le", "\xe2\x89\xa4"},
    {"lfloor", "\xe2\x8c\x8a"},
    {"lowast", "\xe2\x88\x97"},
    {"loz", "\xe2\x97\x8a"},
    {"lrm", "\xe2\x80\x8e"},
    {"lsaquo", "\xe2\x80\xb9"},
    {"lsquo", "\xe2\x80\x98"},
    {"lt", "<"},
    {"macr", "\xc2\xaf"},
    {"mdash", "\xe2\x80\x94"},
    {"micro", "\xc2\xb5"},
    {"middot", "\xc2\xb7"},
    {"minus", "\xe2\x88\x92"},
    {"mu", "\xce\xbc"}, {"Mu", "\xce\x9c"},
    {"nabla", "\xe2\x88\x87"},
    {"nbsp", "\xc2\xa0"},
    {"ndash", "\xe2\x80\x93"},
    {"ne", "\xe2\x89\xa0"},
    {"ni", "\xe2\x88\x8b"},
    {"not", "\xc2\xac"},
    {"ntilde", "\xc3\xb1"}, {"Ntilde", "\xc3\x91"},
    {"Nu", "\xce\x9d"}, {"nu", "\xce\xbd"},
    {"Oacute", "\xc3\x93"}, {"oacute", "\xc3\xb3"},
    {"Ocirc", "\xc3\x94"}, {"ocirc", "\xc3\xb4"},
    {"Ograve", "\xc3\x92"}, {"ograve", "\xc3\xb2"},
    {"oline", "\xe2\x80\xbe"},
    {"Omega", "\xce\xa9"}, {"omega", "\xcf\x89"},
    {"Omicron", "\xce\x9f"}, {"omicron", "\xce\xbf"},
    {"oplus", "\xe2\x8a\x95"},
    {"or", "\xe2\x88\xa8"},
    {"ordf", "\xc2\xaa"}, {"ordm", "\xc2\xba"},
    {"oslash", "\xc3\xb8"}, {"Oslash", "\xc3\x98"},
    {"Otilde", "\xc3\x95"}, {"otilde", "\xc3\xb5"},
    {"otimes", "\xe2\x8a\x97"},
    {"Ouml", "\xc3\x96"}, {"ouml", "\xc3\xb6"},
    {"para", "\xc2\xb6"},
    {"part", "\xe2\x88\x82"},
    {"percnt", "%"},
    {"permil", "\xe2\x80\xb0"},
    {"perp", "\xe2\x8a\xa5"},
    {"Phi", "\xce\xa6"}, {"phi", "\xcf\x86"},
    {"Pi", "\xce\xa0"}, {"pi", "\xcf\x80"},
    {"piv", "\xcf\x96"},
    {"plusmn", "\xc2\xb1"},
    {"pound", "\xc2\xa3"},
    {"prime", "\xe2\x80\xb2"}, {"Prime", "\xe2\x80\xb3"},
    {"prod", "\xe2\x88\x8f"},
    {"prop", "\xe2\x88\x9d"},
    {"Psi", "\xce\xa8"}, {"psi", "\xcf\x88"},
    {"quot", "\""},
    {"radic", "\xe2\x88\x9a"},
    {"rang", "\xe2\x8c\xaa"},
    {"raquo", "\xc2\xbb"},
    {"rarr", "\xe2\x86\x92"}, {"rArr", "\xe2\x87\x92"},
    {"rceil", "\xe2\x8c\x89"},
    {"rdquo", "\xe2\x80\x9d"},
    {"real", "\xe2\x84\x9c"},
    {"reg", "\xc2\xae"},
    {"rfloor", "\xe2\x8c\x8b"},
    {"Rho", "\xce\xa1"}, {"rho", "\xcf\x81"},
    {"rlm", "\xe2\x80\x8f"},
    {"rsaquo", "\xe2\x80\xba"},
    {"rsquo", "\xe2\x80\x99"},
    {"sbquo", "\xe2\x80\x9a"},
    {"Scaron", "\xc5\xa0"}, {"scaron", "\xc5\xa1"},
    {"sdot", "\xe2\x8b\x85"},
    {"sect", "\xc2\xa7"},
    {"shy", "\xc2\xad"},
    {"Sigma", "\xce\xa3"}, {"sigma", "\xcf\x83"},
    {"sigmaf", "\xcf\x82"},
    {"sim", "\xe2\x88\xbc"},
    {"spades", "\xe2\x99\xa0"},
    {"sub", "\xe2\x8a\x82"}, {"sube", "\xe2\x8a\x86"},
    {"sum", "\xe2\x88\x91"},
    {"sup", "\xe2\x8a\x83"}, {"supe", "\xe2\x8a\x87"},
    {"sup1", "\xc2\xb9"}, {"sup2", "\xc2\xb2"}, {"sup3", "\xc2\xb3"},
    {"szlig", "\xc3\x9f"},
    {"Tau", "\xce\xa4"}, {"tau", "\xcf\x84"},
    {"there4", "\xe2\x88\xb4"},
    {"Theta", "\xce\x98"}, {"theta", "\xce\xb8"},
    {"thetasym", "\xcf\x91"},
    {"thinsp", "\xe2\x80\x89"},
    {"times", "\xc3\x97"},
    {"TRADE", "\xe2\x84\xa2"}, {"trade", "\xe2\x84\xa2"},
    {"Uacute", "\xc3\x9a"}, {"uacute", "\xc3\xba"},
    {"uarr", "\xe2\x86\x91"}, {"uArr", "\xe2\x87\x91"},
    {"Ucirc", "\xc3\x9b"}, {"ucirc", "\xc3\xbb"},
    {"Ugrave", "\xc3\x99"}, {"ugrave", "\xc3\xb9"},
    {"uml", "\xc2\xa8"},
    {"upsilon", "\xcf\x85"}, {"Upsilon", "\xcf\x92"},
    {"Uuml", "\xc3\x9c"}, {"uuml", "\xc3\xbc"},
    {"weierp", "\xe2\x84\x98"},
    {"Xi", "\xce\x9e"}, {"xi", "\xce\xbe"},
    {"Yacute", "\xc3\x9d"}, {"yacute", "\xc3\xbd"},
    {"yen", "\xc2\xa5"},
    {"yuml", "\xc3\xbf"}, {"Yuml", "\xc5\xb8"},
    {"Zeta", "\xce\x96"}, {"zeta", "\xce\xb6"},
    {"zwj", "\xe2\x80\x8d"},
    {"zwnj", "\xe2\x80\x8c"},
};

// The table above is written grouped by category (readability), NOT in
// strict byte order — so sort once on first use and binary-search that.
static const NamedEntity* findNamedEntity(const std::string& name) {
    static const std::vector<NamedEntity> sorted = [] {
        std::vector<NamedEntity> v(std::begin(kEntities), std::end(kEntities));
        std::sort(v.begin(), v.end(),
                  [](const NamedEntity& a, const NamedEntity& b) {
                      return std::strcmp(a.name, b.name) < 0;
                  });
        return v;
    }();
    int lo = 0, hi = (int)sorted.size() - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int c = name.compare(sorted[mid].name);
        if (c == 0) return &sorted[mid];
        if (c < 0) hi = mid - 1; else lo = mid + 1;
    }
    return nullptr;
}

// Decode common HTML entities: a wide named-entity table plus numeric
// character references (&#NNN; or &#xHH;). Legacy semicolon-less forms
// (&amp &lt &gt &quot &copy &reg &nbsp) are honored ONLY when followed
// by a character that cannot extend the name (whitespace, '<', '&',
// '=', end of input) — matching the practical intent without mangling
// words like "&timing" into "<iming".
static std::string decodeEntities(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') { out += s[i]; continue; }

        size_t semi = s.find(';', i);
        if (semi != std::string::npos && semi - i <= 12) {
            std::string ent = s.substr(i, semi - i + 1);  // includes ';'

            auto emit = [&](const std::string& repl) {
                out += repl;
                i = semi;  // skip past ';'
            };

            if (ent.size() > 3 && ent[1] == '#') {
                // Numeric: &#NNN; or &#xHH;
                std::string body = ent.substr(2, ent.size() - 3);  // strip &# and ;
                unsigned long code = 0;
                bool ok = !body.empty();
                if (ok) {
                    try {
                        if (body[0] == 'x' || body[0] == 'X')
                            code = std::stoul(body.substr(1), nullptr, 16);
                        else
                            code = std::stoul(body, nullptr, 10);
                    } catch (...) { ok = false; }
                }
                if (ok) {
                    // Encode to UTF-8.
                    if (code < 0x80) {
                        out += char(code);
                    } else if (code < 0x800) {
                        out += char(0xC0 | (code >> 6));
                        out += char(0x80 | (code & 0x3F));
                    } else if (code < 0x10000) {
                        out += char(0xE0 | (code >> 12));
                        out += char(0x80 | ((code >> 6) & 0x3F));
                        out += char(0x80 | (code & 0x3F));
                    } else if (code < 0x110000) {
                        out += char(0xF0 | (code >> 18));
                        out += char(0x80 | ((code >> 12) & 0x3F));
                        out += char(0x80 | ((code >> 6) & 0x3F));
                        out += char(0x80 | (code & 0x3F));
                    }
                    i = semi;
                } else {
                    out += '&';
                }
                continue;
            }

            if (const NamedEntity* ne = findNamedEntity(ent.substr(1, ent.size() - 2))) {
                emit(ne->utf8);
                continue;
            }
            // Unknown named entity — fall through to the literal-'&'
            // handling below (also covers entities longer than the
            // 12-char window, e.g. the huge HTML5 names).
        }

        // Legacy semicolon-less form? Only the classic set, only when a
        // non-name character follows (or the string ends).
        static const char* kLegacy[] = {"amp", "lt", "gt", "quot",
                                        "copy", "reg", "nbsp", nullptr};
        if (s.size() > i) {
            for (int k = 0; kLegacy[k]; ++k) {
                size_t n = std::strlen(kLegacy[k]);
                if (i + 1 + n > s.size()) continue;
                if (s.compare(i + 1, n, kLegacy[k]) != 0) continue;
                char next = (i + 1 + n < s.size()) ? s[i + 1 + n] : ' ';
                if (std::isalnum((unsigned char)next) || next == ';') continue;
                static const char* kRepl[] = {"&", "<", ">", "\"",
                                              "\xc2\xa9", "\xc2\xae", "\xc2\xa0"};
                out += kRepl[k];
                i += n;
                goto handled;
            }
        }
        out += '&';
        handled:;
    }
    return out;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    return s;
}

std::map<std::string, std::string> parseAttrs(const std::string& inside) {
    std::map<std::string, std::string> out;
    size_t i = 0;
    const size_t n = inside.size();
    while (i < n) {
        while (i < n && std::isspace((unsigned char)inside[i])) i++;
        size_t start = i;
        while (i < n && !std::isspace((unsigned char)inside[i]) &&
               inside[i] != '=' && inside[i] != '/' && inside[i] != '>') i++;
        if (i == start) break;
        std::string name = lower(inside.substr(start, i - start));

        while (i < n && std::isspace((unsigned char)inside[i])) i++;

        std::string value;
        if (i < n && inside[i] == '=') {
            i++;
            while (i < n && std::isspace((unsigned char)inside[i])) i++;
            if (i < n && (inside[i] == '"' || inside[i] == '\'')) {
                char q = inside[i++];
                size_t vs = i;
                while (i < n && inside[i] != q) i++;
                value = inside.substr(vs, i - vs);
                if (i < n) i++;
            } else {
                size_t vs = i;
                while (i < n && !std::isspace((unsigned char)inside[i]) &&
                       inside[i] != '>' && inside[i] != '/') i++;
                value = inside.substr(vs, i - vs);
            }
        }
        if (!name.empty()) out[name] = decodeEntities(value);
    }
    return out;
}

// Case-insensitive scan for `</tagname` starting at position i.
// Returns the index of the '<' or npos.
static size_t findRawClose(const std::string& s, size_t i, const std::string& tagLower) {
    std::string want = "</" + tagLower;
    std::string chunk;
    size_t n = s.size();
    for (size_t p = i; p + want.size() <= n; ) {
        // Fast path: compare a window in lowercase without allocating
        // for every position — only lower a candidate first char.
        if (std::tolower((unsigned char)s[p]) == want[0]) {
            bool ok = true;
            for (size_t k = 1; k < want.size(); ++k) {
                if (p + k >= n ||
                    std::tolower((unsigned char)s[p + k]) != want[k]) {
                    ok = false;
                    break;
                }
            }
            // Must be followed by whitespace, '>' or '/' to be a real
            // close tag (so `</scriptx>` doesn't match).
            if (ok) {
                char next = (p + want.size() < n) ? s[p + want.size()] : '>';
                if (next == '>' || next == '/' || std::isspace((unsigned char)next))
                    return p;
            }
        }
        ++p;
    }
    return std::string::npos;
}

std::vector<Tok> tokenize(const std::string& html) {
    std::vector<Tok> toks;
    size_t i = 0;
    // When inside <script>/<style> raw text mode this holds the tag name
    // whose `</name>` we're scanning for.
    std::string rawTextTag;
    while (i < html.size()) {
        // RAWTEXT mode: inside <script> or <style> everything up to the
        // matching close tag is ONE text token with NO entity decoding and
        // NO tag parsing. Without this, `if (a < b)` inside JS or a
        // `"</div>"` string literal would corrupt the DOM tree.
        if (!rawTextTag.empty()) {
            if (html[i] != '<') {
                size_t close = findRawClose(html, i, rawTextTag);
                size_t end  = (close == std::string::npos) ? html.size() : close;
                if (end > i) {
                    Tok t;
                    t.kind = TokKind::TEXT;
                    t.text = html.substr(i, end - i);   // raw: keep & < > as-is
                    toks.push_back(t);
                }
                i = end;
                if (close == std::string::npos) break;
                continue;
            }
            // We're at a '<' in raw mode: it must start our close tag
            // (findRawClose told us so) or it's literal text.
            size_t close = findRawClose(html, i, rawTextTag);
            if (close != i) {
                // Not the real close tag — treat '<' as literal text.
                Tok t;
                t.kind = TokKind::TEXT;
                t.text = "<";
                toks.push_back(t);
                i += 1;
                continue;
            }
            // Real close tag: emit it and leave raw mode. Reuse the
            // generic tag-parsing path below by NOT consuming here.
            rawTextTag.clear();
        }

        if (html[i] != '<') {
            size_t j = i;
            while (j < html.size() && html[j] != '<') j++;
            Tok t;
            t.kind = TokKind::TEXT;
            t.text = decodeEntities(html.substr(i, j - i));
            toks.push_back(t);
            i = j;
            continue;
        }

        // Find the tag's real '>'. A '>' inside a quoted attribute value
        // does NOT end the tag — Wikipedia embeds multi-KB JSON in
        // data-mw attributes, and cutting at the first '>' turned the
        // rest of the JSON into visible page text.
        size_t j = i + 1;
        char quote = 0;
        for (; j < html.size(); ++j) {
            char c = html[j];
            if (quote) {
                if (c == quote) quote = 0;
            } else if (c == '"' || c == '\'') {
                quote = c;
            } else if (c == '>') {
                break;
            }
        }
        if (j >= html.size()) break;   // unterminated tag — eat the rest
        std::string inside = html.substr(i + 1, j - i - 1);

        // Comment: scan for the real closing "-->"
        if (inside.size() >= 3 && inside[0] == '!' &&
            inside[1] == '-' && inside[2] == '-') {
            size_t end = html.find("-->", i + 4);
            if (end == std::string::npos) { i = html.size(); break; }
            size_t closeG = end + 2;
            Tok tc;
            tc.kind = TokKind::COMMENT;
            tc.text = html.substr(i, closeG - i + 1);
            toks.push_back(tc);
            i = closeG + 1;
            continue;
        }

        if (!inside.empty() && inside[0] == '!') {
            Tok td;
            td.kind = TokKind::DOCTYPE;
            td.text = inside;
            toks.push_back(td);
            i = j + 1;
            continue;
        }

        // Heuristic: a `<` that's NOT followed by a letter, `/`, or `!`
        // is treated as literal text (e.g. `a < b`). The previous code
        // always tried to parse the rest as a tag, so `a < b` became an
        // empty TEXT "a " followed by a phantom tag named "b".
        if (!inside.empty()) {
            char first = inside[0];
            if (!std::isalpha((unsigned char)first) &&
                first != '/' && first != '!') {
                // Treat the `<` as text and continue from i+1.
                Tok t;
                t.kind = TokKind::TEXT;
                t.text = "<";
                toks.push_back(t);
                i += 1;
                continue;
            }
        }

        bool selfClose = false;
        if (!inside.empty() && inside.back() == '/') {
            selfClose = true;
            inside.pop_back();
        }
        size_t k = 0;
        while (k < inside.size() && !std::isspace((unsigned char)inside[k])) k++;
        std::string name = lower(inside.substr(0, k));
        std::string rest = (k < inside.size()) ? inside.substr(k) : "";

        Tok t;
        t.attrs = parseAttrs(rest);
        t.name  = name;
        if (!name.empty() && name[0] == '/') {
            t.kind = TokKind::CLOSE;
            t.name = name.substr(1);
        } else if (selfClose && name != "script" && name != "style") {
            // Per HTML parsing, the self-closing "/" is ignored on regular
            // elements — but honoring it for style/script would dump their
            // raw CSS/JS bodies onto the page as text (Reddit's block page
            // ships `<STYLE/>`). They always open raw-text mode below.
            t.kind = TokKind::SELFCLOSE;
        } else {
            t.kind = TokKind::OPEN;
            // Enter RAWTEXT mode for script/style so their bodies are
            // tokenized as a single unparsed text token.
            if (name == "script" || name == "style") {
                rawTextTag = name;
            }
        }
        toks.push_back(t);
        i = j + 1;
    }
    Tok e;
    e.kind = TokKind::EOF_;
    toks.push_back(e);
    return toks;
}

} // namespace browser
