#pragma once
#include <string>

namespace browser {

// Parsed URL. Covers what a mini browser needs: scheme://host:port/path?query#frag
struct Url {
    std::string scheme;    // lowercase: "http", "https", "file"; "" for relative refs
    std::string host;      // lowercase hostname (no user/port)
    std::string port;      // "" means scheme default
    std::string path;      // always starts with '/' for absolute URLs
    std::string query;     // WITHOUT the '?'
    std::string fragment;  // WITHOUT the '#'
    bool valid = false;    // false when the input couldn't be parsed at all

    // scheme://host[:port]/path[?query]  (no fragment)
    std::string toString() const;
    // host[:port]
    std::string hostPort() const;
    // scheme://host[:port]
    std::string origin() const;
    // path[?query]
    std::string pathQuery() const;
};

// Parse an absolute URL. Returns a Url with valid=false on garbage.
Url parseUrl(const std::string& url);

// RFC 3986-ish resolution of a (possibly relative) reference against a
// base URL. Handles:
//   "http://a/b"     absolute
//   "//host/path"    protocol-relative
//   "/path"          origin-relative
//   "path", "./path", "../path"   path-relative
//   "?query", "#frag"
// For file bases (or non-URL bases) it falls back to simple path joining.
std::string joinUrl(const std::string& base, const std::string& ref);

// True for http://, https:// and protocol-relative //host/...
bool isRemoteUrl(const std::string& url);

// True when the string looks like a bare domain the user typed, e.g.
// "example.com", "example.com/page", "localhost:8080/x". Used by the
// address bar to decide between https:// prepending and treating the
// input as a local file path.
bool looksLikeDomainName(const std::string& in);

// Address bar normalization:
//   - "about:home"/"about:blank"/"view-source:..." pass through
//   - scheme already present -> unchanged
//   - starts with "/" or "./" or contains spaces / looks like a path -> file
//   - bare domain -> https://domain
std::string normalizeAddressInput(const std::string& in);

// Percent-encode a query-string component (spaces -> %20, '&' '=' etc).
std::string urlEncode(const std::string& s);

// Decode %XX and '+' -> space. Used for display of URLs.
std::string urlDecode(const std::string& s);

} // namespace browser
