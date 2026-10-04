// Minimal JSON parser for small, trusted-shape payloads (Piped API
// responses). Header-only, no dependencies, no exceptions: parse()
// returns false on ANY malformed input instead of throwing, and every
// accessor on a wrong-typed/absent node returns a safe default, so
// call sites never need guards (extractor code stays linear).
//
// Design: a flat node pool. Each object/array owns a contiguous slice
// of a child-index vector; string bytes (values AND object keys) are
// decoded into one shared buffer. One allocation each, no per-node
// news, no recursion depth hazards in the accessor layer.
//
// Supported: objects, arrays, strings (\", \\, \/, \b, \f, \n, \r,
// \t, \uXXXX incl. surrogate pairs), numbers, true/false/null.
// Deliberately rejected: trailing commas, comments, NaN, raw control
// characters — anything RFC-invalid fails the parse.
//
// Usage:
//   minjson::Doc d;
//   if (d.parse(text)) {
//       d["title"].str();                    // "" when absent
//       const minjson::Val vs = d["videoStreams"];
//       for (unsigned i = 0; i < vs.arrSize(); ++i)
//           vs.at(i)["url"].str();           // "" when absent
//   }
//
// A Doc owns its nodes/bytes; keep it alive while using its Vals.
#pragma once

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace browser {
namespace media {
namespace minjson {

class Doc;

class Val {
public:
    enum Kind { NUL, BOOL, NUM, STR, ARR, OBJ };

    Val() : doc_(nullptr), idx_(0) {}

    Kind kind() const;
    bool isNull() const { return kind() == NUL; }
    bool isObj()  const { return kind() == OBJ; }
    bool isArr()  const { return kind() == ARR; }
    bool isStr()  const { return kind() == STR; }
    bool isNum()  const { return kind() == NUM; }
    bool isBool() const { return kind() == BOOL; }

    // Safe accessors — wrong kind or absent node yields defaults.
    const char* str() const;       // "" unless string
    size_t      len() const;       // decoded byte length (0 unless string)
    double      num() const;       // 0 unless number
    long long   integer() const;   // trunc'd number, 0 unless number
    bool        boolean() const;   // false unless bool
    unsigned    arrSize() const;   // array length (0 unless array)

    // Object member (absent / non-object -> NUL Val).
    Val operator[](const char* key) const;
    // Array element (out of range / non-array -> NUL Val).
    Val at(unsigned i) const;

private:
    friend class Doc;
    Val(const Doc* d, uint32_t idx) : doc_(d), idx_(idx) {}
    const Doc* doc_;
    uint32_t   idx_;
};

class Doc {
public:
    Doc() = default;

    // Parse [text, text+len). On success operator bool() is true and
    // root() / d["key"] are usable; on failure all accessors remain
    // safe and errorAt() gives the failing byte offset.
    bool parse(const char* text, size_t len);
    bool parse(const std::string& s) { return parse(s.data(), s.size()); }

    const Val root() const { return Val(this, rootIdx_); }
    explicit operator bool() const { return ok_; }
    size_t errorAt() const { return errAt_; }

    // d["key"] sugar for root()["key"].
    Val operator[](const char* key) const { return root()[key]; }

private:
    friend class Val;

    struct Node {
        Val::Kind k    = Val::NUL;
        uint32_t  kid0 = 0;    // OBJ/ARR: first slot in kids_
        uint32_t  nkid = 0;    // OBJ/ARR: child count
        uint32_t  off  = 0;    // STR: byte offset in buf_
        uint32_t  len  = 0;    // STR: byte length
        // OBJ children only: key span inside buf_.
        uint32_t  koff = 0;
        uint32_t  klen = 0;
        double    num  = 0.0;
        bool      b    = false;
    };

    const Node* node(uint32_t i) const {
        return i < nodes_.size() ? &nodes_[i] : nullptr;
    }
    Val child(uint32_t self, const char* key) const;
    Val elem(uint32_t self, unsigned i) const;

    bool decodeString(const char*& pos, const char* end,
                      uint32_t& off, uint32_t& len);
    bool parseValue(const char*& pos, const char* end, uint32_t& outIdx);
    bool fail(const char* pos) {
        if (!ok_) errAt_ = (size_t)(pos - src_);
        return false;
    }
    static bool litEq(const char* p, const char* end, const char* lit) {
        while (*lit && p < end) {
            if (*p++ != *lit++) return false;
        }
        return *lit == '\0';
    }
    static void skipWs(const char*& pos, const char* end) {
        while (pos < end &&
               (*pos == ' ' || *pos == '\t' || *pos == '\n' ||
                *pos == '\r'))
            ++pos;
    }

    bool ok_ = false;
    const char* src_ = nullptr;
    std::vector<Node>   nodes_;
    std::vector<uint32_t> kids_;   // child indices (arrays + objects)
    std::string         buf_;      // decoded string/key bytes
    uint32_t rootIdx_ = 0;
    size_t errAt_ = 0;
};

// ---------------------------------------------------------------------------
// Implementation
// ---------------------------------------------------------------------------

inline Val::Kind Val::kind() const {
    const Doc::Node* n = doc_ ? doc_->node(idx_) : nullptr;
    return n ? n->k : NUL;
}

inline const char* Val::str() const {
    const Doc::Node* n = doc_ ? doc_->node(idx_) : nullptr;
    return (n && n->k == STR) ? doc_->buf_.data() + n->off : "";
}

inline size_t Val::len() const {
    const Doc::Node* n = doc_ ? doc_->node(idx_) : nullptr;
    return (n && n->k == STR) ? n->len : 0;
}

inline double Val::num() const {
    const Doc::Node* n = doc_ ? doc_->node(idx_) : nullptr;
    return (n && n->k == NUM) ? n->num : 0.0;
}

inline long long Val::integer() const {
    const Doc::Node* n = doc_ ? doc_->node(idx_) : nullptr;
    return (n && n->k == NUM) ? (long long)n->num : 0;
}

inline bool Val::boolean() const {
    const Doc::Node* n = doc_ ? doc_->node(idx_) : nullptr;
    return (n && n->k == BOOL) ? n->b : false;
}

inline unsigned Val::arrSize() const {
    const Doc::Node* n = doc_ ? doc_->node(idx_) : nullptr;
    return (n && n->k == ARR) ? n->nkid : 0;
}

inline Val Val::operator[](const char* key) const {
    return doc_ ? doc_->child(idx_, key) : Val();
}

inline Val Val::at(unsigned i) const {
    return doc_ ? doc_->elem(idx_, i) : Val();
}

inline Val Doc::child(uint32_t self, const char* key) const {
    const Node* n = node(self);
    if (!n || n->k != Val::OBJ || !key) return Val();
    const size_t klen = std::strlen(key);
    for (uint32_t i = 0; i < n->nkid; ++i) {
        uint32_t ci = kids_[n->kid0 + i];
        const Node* c = node(ci);
        if (c && c->klen == klen &&
            std::memcmp(buf_.data() + c->koff, key, klen) == 0)
            return Val(this, ci);
    }
    return Val();
}

inline Val Doc::elem(uint32_t self, unsigned i) const {
    const Node* n = node(self);
    if (!n || n->k != Val::ARR || i >= n->nkid) return Val();
    return Val(this, kids_[n->kid0 + i]);
}

// Decode the JSON string at `pos` (at the opening quote) into buf_.
inline bool Doc::decodeString(const char*& pos, const char* end,
                              uint32_t& off, uint32_t& len) {
    if (pos >= end || *pos != '"') return fail(pos);
    ++pos;
    off = (uint32_t)buf_.size();
    while (pos < end) {
        unsigned char c = (unsigned char)*pos;
        if (c == '"') {
            ++pos;
            buf_ += '\0';   // sentinel: str() stays NUL-terminated forever
            len = (uint32_t)(buf_.size() - off - 1);
            return true;
        }
        if (c == '\\') {
            ++pos;
            if (pos >= end) return fail(pos);
            char e = *pos++;
            switch (e) {
                case '"':  buf_ += '"';  break;
                case '\\': buf_ += '\\'; break;
                case '/':  buf_ += '/';  break;
                case 'b':  buf_ += '\b'; break;
                case 'f':  buf_ += '\f'; break;
                case 'n':  buf_ += '\n'; break;
                case 'r':  buf_ += '\r'; break;
                case 't':  buf_ += '\t'; break;
                case 'u': {
                    if (end - pos < 4) return fail(pos);
                    unsigned cp = 0;
                    for (int i = 0; i < 4; ++i) {
                        char h = pos[i];
                        cp <<= 4;
                        if (h >= '0' && h <= '9')      cp |= (unsigned)(h - '0');
                        else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
                        else return fail(pos);
                    }
                    pos += 4;
                    if (cp >= 0xD800 && cp <= 0xDBFF && end - pos >= 6 &&
                        pos[0] == '\\' && pos[1] == 'u') {
                        unsigned lo = 0;
                        bool pairOk = true;
                        for (int i = 0; i < 4; ++i) {
                            char h = pos[2 + i];
                            lo <<= 4;
                            if (h >= '0' && h <= '9')      lo |= (unsigned)(h - '0');
                            else if (h >= 'a' && h <= 'f') lo |= (unsigned)(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') lo |= (unsigned)(h - 'A' + 10);
                            else { pairOk = false; break; }
                        }
                        if (pairOk && lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) +
                                 (lo - 0xDC00);
                            pos += 6;
                        }
                    }
                    if (cp < 0x80) {
                        buf_ += (char)cp;
                    } else if (cp < 0x800) {
                        buf_ += (char)(0xC0 | (cp >> 6));
                        buf_ += (char)(0x80 | (cp & 0x3F));
                    } else if (cp < 0x10000) {
                        buf_ += (char)(0xE0 | (cp >> 12));
                        buf_ += (char)(0x80 | ((cp >> 6) & 0x3F));
                        buf_ += (char)(0x80 | (cp & 0x3F));
                    } else {
                        buf_ += (char)(0xF0 | (cp >> 18));
                        buf_ += (char)(0x80 | ((cp >> 12) & 0x3F));
                        buf_ += (char)(0x80 | ((cp >> 6) & 0x3F));
                        buf_ += (char)(0x80 | (cp & 0x3F));
                    }
                    break;
                }
                default:
                    return fail(pos - 1);
            }
            continue;
        }
        if (c < 0x20) return fail(pos);   // raw control char: invalid
        buf_ += (char)c;
        ++pos;
    }
    return fail(pos);                     // unterminated string
}

inline bool Doc::parseValue(const char*& pos, const char* end,
                            uint32_t& outIdx) {
    skipWs(pos, end);
    if (pos >= end) return fail(pos);

    Node nd;

    if (*pos == '{') {
        ++pos;
        nd.k = Val::OBJ;
        // Children are collected locally, then appended to kids_ as ONE
        // contiguous block after the loop: nested containers push their
        // own blocks during recursion, so an incrementally grown window
        // would interleave with the parent's children.
        std::vector<uint32_t> myKids;
        skipWs(pos, end);
        if (pos < end && *pos == '}') {
            ++pos;
        } else {
            for (;;) {
                skipWs(pos, end);
                uint32_t koff, klen;
                if (!decodeString(pos, end, koff, klen)) return false;
                skipWs(pos, end);
                if (pos >= end || *pos != ':') return fail(pos);
                ++pos;
                uint32_t vi;
                if (!parseValue(pos, end, vi)) return false;
                nodes_[vi].koff = koff;   // tag the child with its key
                nodes_[vi].klen = klen;
                myKids.push_back(vi);
                ++nd.nkid;
                skipWs(pos, end);
                if (pos < end && *pos == ',') { ++pos; continue; }
                if (pos < end && *pos == '}') { ++pos; break; }
                return fail(pos);
            }
        }
        outIdx = (uint32_t)nodes_.size();
        nd.kid0 = (uint32_t)kids_.size();
        kids_.insert(kids_.end(), myKids.begin(), myKids.end());
        nodes_.push_back(nd);
        return true;
    }

    if (*pos == '[') {
        ++pos;
        nd.k = Val::ARR;
        std::vector<uint32_t> myKids;   // same block-append discipline
        skipWs(pos, end);
        if (pos < end && *pos == ']') {
            ++pos;
        } else {
            for (;;) {
                uint32_t vi;
                if (!parseValue(pos, end, vi)) return false;
                myKids.push_back(vi);
                ++nd.nkid;
                skipWs(pos, end);
                if (pos < end && *pos == ',') { ++pos; continue; }
                if (pos < end && *pos == ']') { ++pos; break; }
                return fail(pos);
            }
        }
        outIdx = (uint32_t)nodes_.size();
        nd.kid0 = (uint32_t)kids_.size();
        kids_.insert(kids_.end(), myKids.begin(), myKids.end());
        nodes_.push_back(nd);
        return true;
    }

    if (*pos == '"') {
        uint32_t off, len;
        if (!decodeString(pos, end, off, len)) return false;
        nd.k = Val::STR;
        nd.off = off;
        nd.len = len;
        outIdx = (uint32_t)nodes_.size();
        nodes_.push_back(nd);
        return true;
    }

    if (litEq(pos, end, "true") && pos + 4 <= end) {
        pos += 4;
        nd.k = Val::BOOL;
        nd.b = true;
        outIdx = (uint32_t)nodes_.size();
        nodes_.push_back(nd);
        return true;
    }
    if (litEq(pos, end, "false") && pos + 5 <= end) {
        pos += 5;
        nd.k = Val::BOOL;
        nd.b = false;
        outIdx = (uint32_t)nodes_.size();
        nodes_.push_back(nd);
        return true;
    }
    if (litEq(pos, end, "null") && pos + 4 <= end) {
        pos += 4;
        nd.k = Val::NUL;
        outIdx = (uint32_t)nodes_.size();
        nodes_.push_back(nd);
        return true;
    }

    // Number: [-] digits [. digits] [eE[+-] digits]. Strict per RFC:
    // "1." and ".5" are NOT numbers.
    {
        const char* q = pos;
        if (q < end && (*q == '-' || *q == '+')) ++q;
        bool any = false;
        while (q < end && std::isdigit((unsigned char)*q)) { ++q; any = true; }
        if (any && q < end && *q == '.') {
            const char* dot = q;
            ++q;
            if (q < end && std::isdigit((unsigned char)*q)) {
                while (q < end && std::isdigit((unsigned char)*q)) ++q;
            } else {
                q = dot;                  // no digits after '.': exclude it
            }
        }
        if (any && q < end && (*q == 'e' || *q == 'E')) {
            const char* e0 = q;
            ++q;
            if (q < end && (*q == '-' || *q == '+')) ++q;
            bool anyE = false;
            while (q < end && std::isdigit((unsigned char)*q)) { ++q; anyE = true; }
            if (!anyE) q = e0;            // lone 'e' is not part of the number
        }
        if (any) {
            std::string tmp(pos, q - pos);
            char* stop = nullptr;
            double d = std::strtod(tmp.c_str(), &stop);
            pos = q;
            nd.k = Val::NUM;
            nd.num = d;
            outIdx = (uint32_t)nodes_.size();
            nodes_.push_back(nd);
            return true;
        }
    }
    return fail(pos);
}

inline bool Doc::parse(const char* text, size_t len) {
    ok_ = false;
    errAt_ = 0;
    nodes_.clear();
    kids_.clear();
    buf_.clear();
    if (!text) return false;
    src_ = text;
    const char* pos = text;
    const char* end = text + len;
    if (!parseValue(pos, end, rootIdx_)) return false;
    skipWs(pos, end);
    if (pos != end) return fail(pos);     // trailing garbage
    ok_ = true;
    return true;
}

} // namespace minjson
} // namespace media
} // namespace browser
