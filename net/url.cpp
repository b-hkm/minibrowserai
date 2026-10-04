#include "url.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>
#include <vector>

namespace browser {

static std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

static std::string trimStr(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

Url parseUrl(const std::string& raw) {
    Url u;
    std::string s = trimStr(raw);
    if (s.empty()) return u;

    // scheme:
    size_t schemeEnd = s.find("://");
    if (schemeEnd != std::string::npos) {
        u.scheme = toLower(s.substr(0, schemeEnd));
        if (u.scheme.empty()) return u;
        s = s.substr(schemeEnd + 3);
    } else if (s.size() >= 2 && s[0] == '/' && s[1] == '/') {
        u.scheme = "http";  // protocol-relative; caller should keep base scheme
        s = s.substr(2);
    } else {
        return u;  // relative reference — parseUrl is for absolute URLs
    }

    // fragment
    size_t frag = s.find('#');
    if (frag != std::string::npos) {
        u.fragment = s.substr(frag + 1);
        s = s.substr(0, frag);
    }

    // path + query split: authority runs until the first '/'
    size_t slash = s.find('/');
    std::string authority = (slash == std::string::npos) ? s : s.substr(0, slash);
    std::string pathAndQuery = (slash == std::string::npos) ? "" : s.substr(slash);

    // query
    size_t q = pathAndQuery.find('?');
    if (q != std::string::npos) {
        u.query = pathAndQuery.substr(q + 1);
        pathAndQuery = pathAndQuery.substr(0, q);
    }
    u.path = pathAndQuery.empty() ? "/" : pathAndQuery;

    // userinfo@host:port
    size_t at = authority.rfind('@');
    if (at != std::string::npos) authority = authority.substr(at + 1);
    if (!authority.empty()) {
        // IPv6 [..]:port
        if (authority[0] == '[') {
            size_t close = authority.find(']');
            if (close != std::string::npos) {
                u.host = authority.substr(0, close + 1);
                if (close + 2 < authority.size() && authority[close + 1] == ':')
                    u.port = authority.substr(close + 2);
            }
        } else {
            size_t colon = authority.rfind(':');
            if (colon != std::string::npos) {
                u.host = authority.substr(0, colon);
                u.port = authority.substr(colon + 1);
            } else {
                u.host = authority;
            }
        }
    }
    u.host = toLower(u.host);
    u.valid = !u.host.empty();
    return u;
}

std::string Url::hostPort() const {
    if (port.empty()) return host;
    bool isDefault = (scheme == "https" && port == "443") ||
                     (scheme == "http"  && port == "80");
    if (isDefault) return host;
    return host + ":" + port;
}

std::string Url::origin() const {
    if (!valid) return "";
    return scheme + "://" + hostPort();
}

std::string Url::pathQuery() const {
    return query.empty() ? path : path + "?" + query;
}

std::string Url::toString() const {
    if (!valid) return "";
    return origin() + pathQuery();
}

std::string joinUrl(const std::string& base, const std::string& ref) {
    std::string r = trimStr(ref);
    if (r.empty()) return base;

    // Absolute URL — done. A scheme is only "the scheme" when it sits
    // before any '/', '?' or '#'; otherwise it's just text in a path.
    {
        size_t sc = r.find("://");
        if (sc != std::string::npos && sc <= r.find_first_of("/?#")) {
            return r;
        }
    }

    // Special schemes that shouldn't be resolved at all.
    {
        size_t colon = r.find(':');
        if (colon != std::string::npos && colon <= r.find_first_of("/?#")) {
            static const char* kPassSchemes[] = {
                "about:", "view-source:", "javascript:", "data:", "mailto:", nullptr
            };
            for (int i = 0; kPassSchemes[i]; ++i) {
                std::string pfx = kPassSchemes[i];
                if (r.size() >= pfx.size() &&
                    std::equal(pfx.begin(), pfx.end(), r.begin(),
                               [](char a, char b) {
                                   return std::tolower((unsigned char)a) ==
                                          std::tolower((unsigned char)b);
                               })) {
                    return r;
                }
            }
        }
    }

    Url b = parseUrl(base);
    if (!b.valid) return r;  // no usable base — best effort

    // Fragment-only reference: same document.
    if (r[0] == '#') return b.toString() + r;

    // Protocol-relative.
    if (r.size() >= 2 && r[0] == '/' && r[1] == '/') {
        Url ru = parseUrl(b.scheme + ":" + r);
        return ru.valid ? ru.toString() : r;
    }

    // Query-only.
    if (r[0] == '?') return b.origin() + b.path + r;

    std::string mergedPath;
    if (r[0] == '/') {
        mergedPath = r;
    } else {
        std::string dir = b.path.substr(0, b.path.find_last_of('/') + 1);
        if (dir.empty()) dir = "/";
        mergedPath = dir + r;
    }

    // Normalize ./ and ../ segments.
    bool trailingSlash = !mergedPath.empty() && mergedPath.back() == '/';
    std::vector<std::string> parts;
    {
        std::istringstream iss(mergedPath);
        std::string seg;
        while (std::getline(iss, seg, '/')) {
            if (seg.empty() || seg == ".") continue;
            if (seg == "..") {
                if (!parts.empty()) parts.pop_back();
            } else {
                parts.push_back(seg);
            }
        }
    }
    std::string norm;
    for (size_t i = 0; i < parts.size(); ++i) {
        norm += "/" + parts[i];
    }
    if (trailingSlash) norm += "/";
    if (norm.empty()) norm = "/";

    // Split off a query that came along in the reference.
    std::string query;
    {
        size_t q = norm.find('?');
        if (q != std::string::npos) {
            query = norm.substr(q);
            norm = norm.substr(0, q);
        }
        // (a '#' can't appear — refs with fragments were handled above only
        // for the pure-fragment case; handle here too for "?..#..")
        size_t h = query.find('#');
        if (h != std::string::npos) query = query.substr(0, h);
    }

    return b.origin() + norm + query;
}

bool isRemoteUrl(const std::string& url) {
    auto ieq = [&](const char* pfx) {
        size_t n = std::strlen(pfx);
        if (url.size() < n) return false;
        return std::equal(pfx, pfx + n, url.begin(),
                          [](char a, char b) {
                              return std::tolower((unsigned char)a) ==
                                     std::tolower((unsigned char)b);
                          });
    };
    return ieq("http://") || ieq("https://") ||
           (url.size() >= 2 && url[0] == '/' && url[1] == '/' &&
            url.find('.', 2) != std::string::npos);
}

bool looksLikeDomainName(const std::string& in) {
    std::string s = trimStr(in);
    if (s.empty()) return false;
    if (s.find(' ') != std::string::npos) return false;          // "hello world" = search/file
    if (s[0] == '/' || s[0] == '.' || s[0] == '~') return false; // path-ish
    size_t colon = s.find(':');
    if (colon != std::string::npos) {
        // "about:..." / "http:..." have schemes; "localhost:8080" is a domain.
        std::string pre = s.substr(0, colon);
        if (pre.find('.') == std::string::npos && pre != "localhost") {
            return false;  // some other scheme
        }
        if (colon + 1 >= s.size()) return false;
        bool allDigits = true;
        for (size_t i = colon + 1; i < s.size(); ++i) {
            if (s[i] == '/') break;
            if (!std::isdigit((unsigned char)s[i])) { allDigits = false; break; }
        }
        if (!allDigits && s.find('/', colon) == std::string::npos) return false;
    }
    // A file-ish extension on the last segment means it's a local file
    // ("test.html"), not a domain ("test.html" is a valid hostname too,
    // but when typed into a browser address bar users mean the file).
    {
        std::string last = s.substr(s.find_last_of('/') + 1);
        static const char* kFileExt[] = {
            ".html", ".htm", ".txt", ".xml", ".json", ".css", ".js",
            ".png", ".jpg", ".jpeg", ".gif", ".bmp", ".svg", ".ico", nullptr
        };
        for (int i = 0; kFileExt[i]; ++i) {
            size_t n = std::strlen(kFileExt[i]);
            if (last.size() >= n) {
                std::string suf = last.substr(last.size() - n);
                bool eq = std::equal(suf.begin(), suf.end(), kFileExt[i],
                                     [](char a, char b) {
                                         return std::tolower((unsigned char)a) ==
                                                std::tolower((unsigned char)b);
                                     });
                if (eq) return false;
            }
        }
    }
    // Must contain a dot somewhere or be localhost.
    std::string hostPart = s.substr(0, s.find_first_of(":/"));
    if (hostPart == "localhost") return true;
    return hostPart.find('.') != std::string::npos;
}

std::string normalizeAddressInput(const std::string& in) {
    std::string s = trimStr(in);
    if (s.empty()) return s;
    if (looksLikeDomainName(s) && s.find("://") == std::string::npos) {
        return "https://" + s;
    }
    return s;
}

std::string urlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += (char)c;
        } else if (c == ' ') {
            out += '+';
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

std::string urlDecode(const std::string& s) {
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
            hexVal(s[i + 1]) >= 0 && hexVal(s[i + 2]) >= 0) {
            out += (char)(hexVal(s[i + 1]) * 16 + hexVal(s[i + 2]));
            i += 2;
        } else if (s[i] == '+') {
            out += ' ';
        } else {
            out += s[i];
        }
    }
    return out;
}

} // namespace browser
